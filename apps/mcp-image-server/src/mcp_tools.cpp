#include "mcp_tools.hpp"

#include "image_processor.hpp"
#include "openai_image_provider.hpp"
#include "sdcpp_image_provider.hpp"

#include "mcp_message.h"
#include "mcp_tool.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mcdk::image {

namespace {

struct CachedImage {
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

std::map<std::string, std::vector<CachedImage>>& image_cache() {
    static std::map<std::string, std::vector<CachedImage>> cache;
    return cache;
}

std::mutex& image_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string make_job_id() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::filesystem::path utf8_path_from_string(const std::string& value) {
    return std::filesystem::u8path(value);
}

std::string path_to_utf8_string(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path require_absolute_utf8_path(const std::string& value, const char* field_name) {
    if (value.empty()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, std::string(field_name) + " must not be empty");
    }

    std::filesystem::path path = utf8_path_from_string(value).lexically_normal();
    if (!path.is_absolute()) {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            std::string(field_name) + " must be an absolute UTF-8 file path; relative paths are forbidden"
        );
    }
    return path;
}

std::string extension_from_mime(const std::string& mime) {
    if (mime == "image/jpeg") {
        return ".jpg";
    }
    if (mime == "image/webp") {
        return ".webp";
    }
    return ".png";
}

std::string mime_from_path(const std::filesystem::path& path) {
    const std::string ext = path_to_utf8_string(path.extension());
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".JPG" || ext == ".JPEG") {
        return "image/jpeg";
    }
    if (ext == ".webp" || ext == ".WEBP") {
        return "image/webp";
    }
    return "image/png";
}

std::filesystem::path indexed_output_path(std::filesystem::path output_path, std::size_t index, const std::string& mime_type) {
    const std::filesystem::path parent = output_path.parent_path();
    const std::string stem = path_to_utf8_string(output_path.stem());
    const std::string ext = output_path.has_extension() ? path_to_utf8_string(output_path.extension()) : extension_from_mime(mime_type);
    return (parent / std::filesystem::u8path(stem + "-" + std::to_string(index) + ext)).lexically_normal();
}

CachedImage get_cached_image(const std::string& job_id, int image_index) {
    if (job_id.empty()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "jobId/sourceJobId must not be empty");
    }
    if (image_index < 0) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "index/sourceIndex must be >= 0");
    }

    std::lock_guard<std::mutex> lock(image_cache_mutex());
    auto job_it = image_cache().find(job_id);
    if (job_it == image_cache().end()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "cached jobId not found or MCP process was restarted");
    }
    if (static_cast<std::size_t>(image_index) >= job_it->second.size()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "cached image index out of range");
    }
    return job_it->second[static_cast<std::size_t>(image_index)];
}

bool bool_param(const Json& params, const char* key, bool fallback) {
    return params.contains(key) && params[key].is_boolean() ? params[key].get<bool>() : fallback;
}

int int_param(const Json& params, const char* key, int fallback) {
    if (params.contains(key) && params[key].is_number_integer()) {
        return params[key].get<int>();
    }
    if (params.contains(key) && params[key].is_number()) {
        return static_cast<int>(params[key].get<double>());
    }
    return fallback;
}

std::optional<std::string> string_param(const Json& params, const char* key) {
    if (params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return std::nullopt;
}

Json tool_content_text(const std::string& text) {
    return Json::array({Json{{"type", "text"}, {"text", text}}});
}

Json tool_content_with_metadata(const std::string& summary, const Json& metadata) {
    return Json::array({
        Json{{"type", "text"}, {"text", summary}},
        Json{{"type", "text"}, {"text", metadata.dump(2)}}
    });
}

ImageData cached_to_image_data(const CachedImage& cached) {
    ImageData image;
    image.mime_type = cached.mime_type;
    image.bytes = cached.bytes;
    return image;
}

ImageData load_single_input_image(const Json& params) {
    const auto source_job_id = string_param(params, "sourceJobId");
    const auto input_path = string_param(params, "inputPath");
    const auto input_base64 = string_param(params, "inputBase64");
    const int source_count = (source_job_id ? 1 : 0) + (input_path ? 1 : 0) + (input_base64 ? 1 : 0);
    if (source_count != 1) {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            "exactly one of sourceJobId, inputPath, or inputBase64 is required"
        );
    }

    if (source_job_id) {
        return cached_to_image_data(get_cached_image(*source_job_id, int_param(params, "sourceIndex", 0)));
    }

    ImageData image;
    if (input_path) {
        const std::filesystem::path path = require_absolute_utf8_path(*input_path, "inputPath");
        image.mime_type = string_param(params, "inputMimeType").value_or(mime_from_path(path));
        image.bytes = read_binary_file(path_to_utf8_string(path));
        return image;
    }

    image.mime_type = string_param(params, "inputMimeType").value_or("image/png");
    image.bytes = base64_decode_bytes(*input_base64);
    return image;
}

ImageGenerationRequest parse_generation_request(const Json& params, const AppConfig& config) {
    if (!params.contains("prompt") || !params["prompt"].is_string() || params["prompt"].get<std::string>().empty()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "prompt is required");
    }

    ImageGenerationRequest request;
    request.prompt = params["prompt"].get<std::string>();
    request.model = string_param(params, "model").value_or(config.default_model);
    const std::string default_size = (config.protocol == "sdcpp" || config.protocol == "stable-diffusion.cpp")
        ? config.sdcpp.default_size
        : "1024x1024";
    request.size = string_param(params, "size").value_or(default_size);
    request.quality = string_param(params, "quality");
    request.style = string_param(params, "style");
    request.native_transparency = bool_param(params, "nativeTransparency", false);
    request.timeout_seconds = int_param(params, "timeoutSeconds", config.timeout_seconds);

    const int n = int_param(params, "n", 1);
    if (n <= 0 || n > 4) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "n must be between 1 and 4");
    }
    request.n = n;

    if (params.contains("extra") && params["extra"].is_object()) {
        request.extra = params["extra"];
    }

    if (params.contains("provider")) {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            "provider connection config must be passed by environment variables only"
        );
    }

    return request;
}

ImageEditRequest parse_edit_request(const Json& params, const AppConfig& config) {
    if (!params.contains("prompt") || !params["prompt"].is_string() || params["prompt"].get<std::string>().empty()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "prompt is required");
    }

    ImageEditRequest request;
    request.prompt = params["prompt"].get<std::string>();
    request.model = string_param(params, "model").value_or(config.default_model);
    request.size = string_param(params, "size").value_or("1024x1024");
    request.quality = string_param(params, "quality");
    request.native_transparency = bool_param(params, "nativeTransparency", false);
    request.timeout_seconds = int_param(params, "timeoutSeconds", config.timeout_seconds);
    request.input_images.push_back(load_single_input_image(params));

    const int n = int_param(params, "n", 1);
    if (n <= 0 || n > 4) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "n must be between 1 and 4");
    }
    request.n = n;

    if (params.contains("extra") && params["extra"].is_object()) {
        request.extra = params["extra"];
    }

    if (params.contains("provider")) {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            "provider connection config must be passed by environment variables only"
        );
    }

    return request;
}

Json shared_processing_properties() {
    return {
        {"enabled", {
            {"type", "boolean"},
            {"description", "Enable the post-processing pipeline. Set true when using chromaKey, nearestResize, transparentDomainScale, pixelArtCompress, or removeFakeTransparency."}
        }},
        {"chromaKey", {
            {"type", "object"},
            {"description", "Remove a guided solid-color chroma screen background into real alpha. Exact supported fields: enabled:boolean, r:number, g:number, b:number, tolerance:number, softness:number, spillSuppression:number. Defaults to RGB(0,255,0). Choose a key color that does not appear in the asset. spillSuppression is numeric strength, not boolean."},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "Enable chroma-key removal."}}},
                {"r", {{"type", "number"}, {"description", "Chroma key red channel, 0-255. Default 0."}}},
                {"g", {{"type", "number"}, {"description", "Chroma key green channel, 0-255. Default 255."}}},
                {"b", {{"type", "number"}, {"description", "Chroma key blue channel, 0-255. Default 0."}}},
                {"tolerance", {{"type", "number"}, {"description", "Hard removal tolerance. Default 72."}}},
                {"softness", {{"type", "number"}, {"description", "Soft edge tolerance. Default 32."}}},
                {"spillSuppression", {{"type", "number"}, {"description", "Color spill suppression strength. Default 48. Use a number, not true/false."}}}
            }}
        }},
        {"nearestResize", {
            {"type", "object"},
            {"description", "Nearest-neighbor resize options. Exact supported fields: enabled:boolean, targetWidth:number, targetHeight:number. IMPORTANT: use targetWidth/targetHeight; width/height are ignored. For Minecraft pixel art, prefer 16x16 or 32x32; 32x32 is usually the most balanced."},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "Enable nearest-neighbor resizing."}}},
                {"targetWidth", {{"type", "number"}, {"description", "Required positive output width when enabled, e.g. 16, 32, 64, or 128. Do not use width."}}},
                {"targetHeight", {{"type", "number"}, {"description", "Required positive output height when enabled, e.g. 16, 32, 64, or 128. Do not use height."}}}
            }}
        }},
        {"transparentDomainScale", {
            {"type", "object"},
            {"description", "Crop the non-transparent alpha domain, add padding, and fit it into a target canvas. Exact supported fields: enabled:boolean, padding:number, targetWidth:number, targetHeight:number, alphaThreshold:number. Use targetWidth/targetHeight; width/height are ignored."},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "Enable transparent-domain crop and scale."}}},
                {"padding", {{"type", "number"}, {"description", "Transparent padding in output pixels. Default 0."}}},
                {"targetWidth", {{"type", "number"}, {"description", "Required positive output canvas width when enabled, e.g. 32."}}},
                {"targetHeight", {{"type", "number"}, {"description", "Required positive output canvas height when enabled, e.g. 32."}}},
                {"alphaThreshold", {{"type", "number"}, {"description", "Minimum alpha treated as non-transparent for cropping. Default 1."}}}
            }}
        }},
        {"pixelArtCompress", {
            {"type", "object"},
            {"description", "Pixel-art compression/downsampling options. Exact supported fields: enabled:boolean, maxWidth:number, maxHeight:number. IMPORTANT: use maxWidth/maxHeight; targetSize, targetWidth, and targetHeight are ignored for this option. Prefer 16x16 or 32x32 for most Minecraft assets."},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "Enable pixel-art compression."}}},
                {"maxWidth", {{"type", "number"}, {"description", "Maximum output width when enabled, e.g. 16, 32, 64, or 128. Do not use targetSize."}}},
                {"maxHeight", {{"type", "number"}, {"description", "Maximum output height when enabled, e.g. 16, 32, 64, or 128. Do not use targetSize."}}}
            }}
        }},
        {"removeFakeTransparency", {
            {"type", "object"},
            {"description", "Legacy border-color cleanup. Prefer chromaKey for new transparent-background workflows. Exact supported fields: enabled:boolean, tolerance:number, alpha:number."},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "Enable legacy fake-transparency cleanup."}}},
                {"tolerance", {{"type", "number"}, {"description", "Border-color match tolerance. Default 16."}}},
                {"alpha", {{"type", "number"}, {"description", "Alpha value assigned to removed pixels. Default 0."}}}
            }}
        }}
    };
}

mcp::tool build_generate_image_tool() {
    Json processing_properties = shared_processing_properties();

    return mcp::tool_builder("generate_image")
        .with_description("Generate ONE Minecraft game asset per image through the environment-configured OpenAI-compatible image provider. Models behind sub2api-style gateways usually do not return true native alpha even for PNG, so the recommended transparent workflow is: request a flat solid chroma screen via processing.chromaKey.enabled=true, then locally remove that configured key color into alpha. The key color is not required to be green: choose r,g,b to avoid conflicts with the asset's dominant palette and semi-transparent edge colors. Default generation size is 1024x1024; post-process down to 32x32 or 16x16 for Minecraft assets. Base64 is returned only when the final output is <=128x128; larger images must be saved or downsampled first.")
        .with_string_param("prompt", "Image prompt", true)
        .with_string_param("model", "Image model override for this request", false)
        .with_string_param("size", "Optional provider image size. Defaults to 1024x1024 because some gateways reject 512x512; use smaller sizes only when the provider is known to support them.", false)
        .with_number_param("n", "Number of images, 1-4", false)
        .with_string_param("quality", "Provider quality option", false)
        .with_string_param("style", "Provider style option", false)
        .with_boolean_param("nativeTransparency", "Request native PNG alpha parameters if the provider supports them. Most tested image models here returned opaque PNG; prefer processing.chromaKey for transparent output.", false)
        .with_object_param("extra", "Extra provider JSON fields, excluding connection config. Use this for provider-specific native alpha/transparency controls.", Json::object(), false)
        .with_object_param("processing", "Optional image processing pipeline", processing_properties, false)
        .with_boolean_param("returnBase64", "Return image base64 in metadata. Defaults to true for in-memory review.", false)
        .with_boolean_param("saveToFile", "Save image files only when an explicit absolute UTF-8 outputPath is provided. Defaults to false; enable only after self-review accepts the asset.", false)
        .with_string_param("outputPath", "Required absolute UTF-8 output file path/name when saveToFile=true. Relative paths are rejected. For multiple images, an index is appended before the extension.", false)
        .with_boolean_param("selfReviewHint", "Return a visual self-review prompt and base64 payload. Defaults to true.", false)
        .with_number_param("timeoutSeconds", "Per-request timeout seconds, default 400", false)
        .with_open_world_hint(true)
        .build();
}

mcp::tool build_save_cached_image_tool() {
    return mcp::tool_builder("save_cached_image")
        .with_description("Save an image previously generated in memory by generate_image without calling the image model again. Use this after LLM self-review accepts the cached asset.")
        .with_string_param("jobId", "The jobId returned by generate_image", true)
        .with_number_param("index", "Image index in the cached job, default 0", false)
        .with_string_param("outputPath", "Required absolute UTF-8 output file path. Relative paths are rejected.", true)
        .with_read_only_hint(false)
        .build();
}

mcp::tool build_process_image_tool() {
    Json processing_properties = shared_processing_properties();

    return mcp::tool_builder("process_image")
        .with_description("Process a local image file or base64 image with Minecraft-oriented nearest-neighbor, transparent-domain, 16/32/64/128 pixel-art sizing, and fake-transparency cleanup operations.")
        .with_string_param("inputPath", "Absolute UTF-8 input image file path. Relative paths are rejected.", false)
        .with_string_param("inputBase64", "Input image base64", false)
        .with_string_param("outputPath", "Required absolute UTF-8 output file path. Relative paths are rejected.", true)
        .with_object_param("processing", "Image processing pipeline", processing_properties, true)
        .with_boolean_param("returnBase64", "Return output image base64", false)
        .with_read_only_hint(false)
        .build();
}

mcp::tool build_edit_image_tool() {
    Json processing_properties = shared_processing_properties();

    return mcp::tool_builder("edit_image")
        .with_description("Edit an existing image through the environment-configured OpenAI-compatible image edit endpoint. Input can come from a cached generate_image/edit_image jobId, an absolute UTF-8 inputPath, or inputBase64. For transparent output, prefer processing.chromaKey.enabled=true so the edit is guided onto a flat solid chroma screen and then locally keyed to alpha. The key color is configurable and should be selected to contrast with the image style rather than always using green. Base64 is returned only when the final output is <=128x128.")
        .with_string_param("prompt", "Edit instruction prompt", true)
        .with_string_param("model", "Image model override for this request", false)
        .with_string_param("sourceJobId", "Cached source jobId returned by generate_image or edit_image", false)
        .with_number_param("sourceIndex", "Cached source image index, default 0", false)
        .with_string_param("inputPath", "Absolute UTF-8 input image file path. Relative paths are rejected.", false)
        .with_string_param("inputBase64", "Input image base64", false)
        .with_string_param("inputMimeType", "MIME type for inputBase64, default image/png", false)
        .with_string_param("size", "Optional provider image size. Defaults to 1024x1024 because some gateways reject 512x512; use post-processing to downsample.", false)
        .with_number_param("n", "Number of edited images, 1-4", false)
        .with_string_param("quality", "Provider quality option", false)
        .with_boolean_param("nativeTransparency", "Request native PNG alpha parameters if the provider supports them. Prefer processing.chromaKey for transparent output with tested non-alpha models.", false)
        .with_object_param("extra", "Extra provider JSON fields, excluding connection config. Use this for provider-specific edit parameters.", Json::object(), false)
        .with_object_param("processing", "Optional image processing pipeline", processing_properties, false)
        .with_boolean_param("returnBase64", "Return image base64 in metadata. Defaults to true for in-memory review.", false)
        .with_boolean_param("saveToFile", "Save image files only when an explicit absolute UTF-8 outputPath is provided. Defaults to false.", false)
        .with_string_param("outputPath", "Required absolute UTF-8 output file path/name when saveToFile=true. Relative paths are rejected. For multiple images, an index is appended before the extension.", false)
        .with_boolean_param("selfReviewHint", "Return a visual self-review prompt and base64 payload. Defaults to true.", false)
        .with_number_param("timeoutSeconds", "Per-request timeout seconds, default 400", false)
        .with_open_world_hint(true)
        .build();
}

std::string minecraft_asset_prompt(const std::string& user_prompt) {
    return user_prompt
        + "\n\nStrict Minecraft asset rules: generate exactly ONE standalone game asset in this image. "
          "Do not create a spritesheet, grid, collage, collection, multiple items, hands, characters, labels, text, watermark, or UI. "
          "Use a clean isolated source composition without preview boards, background patterns, decorative tiles, or material swatches. "
          "Use a composition that can be downsampled to Minecraft texture sizes; prefer 32x32 for balanced item textures, 16x16 for vanilla-like/simple assets, and reserve 64x64/128x128 for high-complexity assets only. "
          "Keep a clean readable silhouette, centered object, crisp pixel-art edges, and low-noise colors.";
}

int clamp_color_channel(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

std::string chroma_key_color_text(const ImageProcessingOptions& processing) {
    std::ostringstream stream;
    stream << "RGB("
           << clamp_color_channel(processing.chroma_key_r) << ","
           << clamp_color_channel(processing.chroma_key_g) << ","
           << clamp_color_channel(processing.chroma_key_b) << ")";
    return stream.str();
}

std::string chroma_key_prompt(const std::string& prompt, const ImageProcessingOptions& processing) {
    if (!processing.chroma_key_enabled) {
        return prompt;
    }
    const std::string key_color = chroma_key_color_text(processing);
    return prompt
        + "\n\nChroma-key transparency workflow: render the asset in front of a perfectly flat solid chroma-key screen background " + key_color + ". "
          "The chroma-key color is not required to be green; it should be configured to strongly contrast with the asset's dominant palette and to avoid colors that may appear in semi-transparent pixels or glow edges. "
          "Use exactly this configured key color as a single uniform solid background, with no gradients, shadows, texture, border, checkerboard, glow, vignette, ground plane, or environmental lighting on it. "
          "Keep the object fully separate from the chroma-key screen. Avoid this key color and nearby hues inside the asset unless absolutely necessary.";
}

bool image_too_large_for_base64(const ImageData& image, int max_dimension = 128) {
    try {
        ImageBuffer buffer = decode_image_rgba(image.bytes);
        return buffer.width > max_dimension || buffer.height > max_dimension;
    } catch (...) {
        return true;
    }
}

std::string provider_display_name(const AppConfig& config) {
    return (config.protocol == "sdcpp" || config.protocol == "stable-diffusion.cpp") ? "sdcpp" : config.protocol;
}

Json handle_generate_image(const Json& params, const AppConfig& config) {
    const auto started = std::chrono::steady_clock::now();
    const std::string job_id = make_job_id();
    const bool return_base64 = bool_param(params, "returnBase64", true);
    const bool save_to_file = bool_param(params, "saveToFile", false);
    const bool self_review_hint = bool_param(params, "selfReviewHint", true);
    const bool needs_final_png = return_base64 || self_review_hint || save_to_file;

    std::unique_ptr<ImageProvider> provider;
    if (config.protocol == "sdcpp" || config.protocol == "stable-diffusion.cpp") {
        provider = std::make_unique<StableDiffusionCppProvider>(config);
    } else {
        provider = std::make_unique<OpenAIImageProvider>(config);
    }
    ImageGenerationRequest request = parse_generation_request(params, config);
    ImageProcessingOptions processing = parse_processing_options(params);
    request.prompt = chroma_key_prompt(minecraft_asset_prompt(request.prompt), processing);
    processing.force_png_output = needs_final_png || request.native_transparency || processing.chroma_key_enabled;
    ImageGenerationResult generated = provider->generate(request);

    Json metadata = Json::object();
    metadata["jobId"] = job_id;
    metadata["provider"] = provider_display_name(config);
    metadata["model"] = request.model;
    metadata["nativeTransparency"] = request.native_transparency;
    metadata["transparencyWorkflow"] = processing.chroma_key_enabled ? "chroma_key_green_screen" : (request.native_transparency ? "native_provider_alpha_requested" : "none");
    if (provider_display_name(config) == "sdcpp") {
        metadata["sdcpp"] = generated.raw_response;
    }
    metadata["images"] = Json::array();

    std::vector<CachedImage> cache_entry;

    std::size_t index = 0;
    for (const ImageData& source_image : generated.images) {
        ImageData image = process_image_data(source_image, processing);
        cache_entry.push_back(CachedImage{image.mime_type, image.bytes});
        Json image_meta = Json::object();
        image_meta["index"] = index;
        image_meta["mimeType"] = image.mime_type;
        image_meta["bytes"] = image.bytes.size();

        if (save_to_file) {
            const auto requested_output = string_param(params, "outputPath");
            if (!requested_output) {
                throw mcp::mcp_exception(
                    mcp::error_code::invalid_params,
                    "outputPath is required when saveToFile=true; it must be an absolute UTF-8 file path"
                );
            }

            std::filesystem::path output_path = require_absolute_utf8_path(*requested_output, "outputPath");
            if (generated.images.size() > 1) {
                output_path = indexed_output_path(output_path, index, image.mime_type);
            }
            write_binary_file(path_to_utf8_string(output_path), image.bytes);
            image_meta["filePath"] = path_to_utf8_string(output_path);
        }

        const bool base64_blocked_by_size = image_too_large_for_base64(image);
        if ((return_base64 || self_review_hint) && !base64_blocked_by_size) {
            image_meta["base64"] = base64_encode_bytes(image.bytes);
        } else if (return_base64 || self_review_hint) {
            image_meta["base64Rejected"] = "output image exceeds 128x128; save to file or enable processing.nearestResize/pixelArtCompress first";
        }

        image_meta["processing"] = {
            {"enabled", processing.enabled},
            {"chromaKeyApplied", processing.enabled && processing.chroma_key_enabled},
            {"chromaKeyColor", {processing.chroma_key_r, processing.chroma_key_g, processing.chroma_key_b}},
            {"nearestResizeApplied", processing.enabled && processing.nearest_resize_enabled},
            {"transparentDomainScaleApplied", processing.enabled && processing.transparent_domain_scale_enabled},
            {"autoTransparentDomainScaleApplied", processing.enabled && processing.chroma_key_enabled && !processing.transparent_domain_scale_enabled && processing.nearest_resize_enabled},
            {"pixelArtCompressApplied", processing.enabled && processing.pixel_art_compress_enabled},
            {"removeFakeTransparencyApplied", processing.enabled && processing.remove_fake_transparency_enabled}
        };
        metadata["images"].push_back(std::move(image_meta));
        ++index;
    }

    {
        std::lock_guard<std::mutex> lock(image_cache_mutex());
        image_cache()[job_id] = std::move(cache_entry);
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    metadata["elapsedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (self_review_hint && !metadata["images"].empty() && metadata["images"][0].contains("base64")) {
        metadata["selfReview"] = {
            {"enabled", true},
            {"prompt", "请作为 Minecraft 美术审查员，检查该图是否满足：1）只有一个独立素材，不是四宫格/合集/spritesheet；2）如需要透明/半透明，必须来自原生 alpha/provider 参数，而不是画出来的棋盘格、有色色块或提示词伪透明；3）优先检查 32x32 下是否轮廓清晰，简单/原版风素材再检查 16x16，只有高复杂度素材才建议 64x64/128x128；4）低噪声、主体居中、适合作为游戏贴图。若不满足，请输出下一轮改进建议；若问题是透明/半透明，请建议启用 nativeTransparency 或 provider extra 字段，而不是仅修改提示词；只有确认完美后才建议调用 save_cached_image 落盘。"},
            {"image", {
                {"mimeType", metadata["images"][0]["mimeType"]},
                {"base64", metadata["images"][0]["base64"]}
            }}
        };
    }

    metadata["workflow"] = {
        {"inMemoryReviewFirst", true},
        {"savedToFile", save_to_file},
        {"finalizeInstruction", "默认仅返回 base64 供 LLM 自审，并将图像缓存在当前 MCP 进程内；确认素材完美后，调用 save_cached_image(jobId,index) 直接保存缓存图，避免重新生成。"}
    };

    return tool_content_with_metadata("生成完成：" + std::to_string(metadata["images"].size()) + " 张图像，默认以内存 base64 形式供自审，并已缓存可供 save_cached_image 直接保存。", metadata);
}

Json handle_save_cached_image(const Json& params, const AppConfig& config) {
    const auto job_id = string_param(params, "jobId");
    if (!job_id || job_id->empty()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "jobId is required");
    }

    const int image_index = int_param(params, "index", 0);
    if (image_index < 0) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "index must be >= 0");
    }

    CachedImage cached = get_cached_image(*job_id, image_index);

    const auto requested_output = string_param(params, "outputPath");
    if (!requested_output) {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            "outputPath is required; it must be an absolute UTF-8 file path"
        );
    }
    std::filesystem::path output_path = require_absolute_utf8_path(*requested_output, "outputPath");
    write_binary_file(path_to_utf8_string(output_path), cached.bytes);

    Json metadata = {
        {"jobId", *job_id},
        {"index", image_index},
        {"mimeType", cached.mime_type},
        {"bytes", cached.bytes.size()},
        {"filePath", path_to_utf8_string(output_path)},
        {"reusedCachedImage", true}
    };

    return tool_content_with_metadata("已保存缓存图像，没有重新调用图像模型。", metadata);
}

Json handle_process_image(const Json& params, const AppConfig& config) {
    const bool return_base64 = bool_param(params, "returnBase64", false);
    const auto input_path = string_param(params, "inputPath");
    const auto input_base64 = string_param(params, "inputBase64");
    if ((!input_path && !input_base64) || (input_path && input_base64)) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "exactly one of inputPath or inputBase64 is required");
    }

    ImageData input;
    input.mime_type = "image/png";
    input.bytes = input_path
        ? read_binary_file(path_to_utf8_string(require_absolute_utf8_path(*input_path, "inputPath")))
        : base64_decode_bytes(*input_base64);

    ImageProcessingOptions processing = parse_processing_options(params);
    processing.enabled = true;
    ImageData output = process_image_data(input, processing);

    Json metadata = Json::object();
    metadata["mimeType"] = output.mime_type;
    metadata["bytes"] = output.bytes.size();

    const auto requested_output = string_param(params, "outputPath");
    if (!requested_output) {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            "outputPath is required; it must be an absolute UTF-8 file path"
        );
    }
    std::filesystem::path output_path = require_absolute_utf8_path(*requested_output, "outputPath");
    write_binary_file(path_to_utf8_string(output_path), output.bytes);
    metadata["filePath"] = path_to_utf8_string(output_path);

    if (return_base64) {
        metadata["base64"] = base64_encode_bytes(output.bytes);
    }

    return tool_content_with_metadata("图像处理完成。", metadata);
}

Json handle_edit_image(const Json& params, const AppConfig& config) {
    const auto started = std::chrono::steady_clock::now();
    const std::string job_id = make_job_id();
    const bool return_base64 = bool_param(params, "returnBase64", true);
    const bool save_to_file = bool_param(params, "saveToFile", false);
    const bool self_review_hint = bool_param(params, "selfReviewHint", true);
    const bool needs_final_png = return_base64 || self_review_hint || save_to_file;

    if (config.protocol == "sdcpp" || config.protocol == "stable-diffusion.cpp") {
        throw mcp::mcp_exception(
            mcp::error_code::invalid_params,
            "edit_image is not supported by the stable-diffusion.cpp provider yet; use generate_image or switch MCDK_IMAGE_PROTOCOL=openai"
        );
    }

    OpenAIImageProvider provider(config);
    ImageEditRequest request = parse_edit_request(params, config);
    ImageProcessingOptions processing = parse_processing_options(params);
    request.prompt = chroma_key_prompt(minecraft_asset_prompt(request.prompt), processing);
    processing.force_png_output = needs_final_png || request.native_transparency || processing.chroma_key_enabled;
    ImageGenerationResult edited = provider.edit(request);

    Json metadata = Json::object();
    metadata["jobId"] = job_id;
    metadata["provider"] = provider_display_name(config);
    metadata["model"] = request.model;
    metadata["nativeTransparency"] = request.native_transparency;
    metadata["transparencyWorkflow"] = processing.chroma_key_enabled ? "chroma_key_green_screen" : (request.native_transparency ? "native_provider_alpha_requested" : "none");
    metadata["inputImageCount"] = request.input_images.size();
    metadata["images"] = Json::array();

    std::vector<CachedImage> cache_entry;

    std::size_t index = 0;
    for (const ImageData& source_image : edited.images) {
        ImageData image = process_image_data(source_image, processing);
        cache_entry.push_back(CachedImage{image.mime_type, image.bytes});
        Json image_meta = Json::object();
        image_meta["index"] = index;
        image_meta["mimeType"] = image.mime_type;
        image_meta["bytes"] = image.bytes.size();

        if (save_to_file) {
            const auto requested_output = string_param(params, "outputPath");
            if (!requested_output) {
                throw mcp::mcp_exception(
                    mcp::error_code::invalid_params,
                    "outputPath is required when saveToFile=true; it must be an absolute UTF-8 file path"
                );
            }

            std::filesystem::path output_path = require_absolute_utf8_path(*requested_output, "outputPath");
            if (edited.images.size() > 1) {
                output_path = indexed_output_path(output_path, index, image.mime_type);
            }
            write_binary_file(path_to_utf8_string(output_path), image.bytes);
            image_meta["filePath"] = path_to_utf8_string(output_path);
        }

        const bool base64_blocked_by_size = image_too_large_for_base64(image);
        if ((return_base64 || self_review_hint) && !base64_blocked_by_size) {
            image_meta["base64"] = base64_encode_bytes(image.bytes);
        } else if (return_base64 || self_review_hint) {
            image_meta["base64Rejected"] = "output image exceeds 128x128; save to file or enable processing.nearestResize/pixelArtCompress first";
        }

        image_meta["processing"] = {
            {"enabled", processing.enabled},
            {"chromaKeyApplied", processing.enabled && processing.chroma_key_enabled},
            {"chromaKeyColor", {processing.chroma_key_r, processing.chroma_key_g, processing.chroma_key_b}},
            {"nearestResizeApplied", processing.enabled && processing.nearest_resize_enabled},
            {"transparentDomainScaleApplied", processing.enabled && processing.transparent_domain_scale_enabled},
            {"autoTransparentDomainScaleApplied", processing.enabled && processing.chroma_key_enabled && !processing.transparent_domain_scale_enabled && processing.nearest_resize_enabled},
            {"pixelArtCompressApplied", processing.enabled && processing.pixel_art_compress_enabled},
            {"removeFakeTransparencyApplied", processing.enabled && processing.remove_fake_transparency_enabled}
        };
        metadata["images"].push_back(std::move(image_meta));
        ++index;
    }

    {
        std::lock_guard<std::mutex> lock(image_cache_mutex());
        image_cache()[job_id] = std::move(cache_entry);
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    metadata["elapsedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (self_review_hint && !metadata["images"].empty() && metadata["images"][0].contains("base64")) {
        metadata["selfReview"] = {
            {"enabled", true},
            {"prompt", "请作为 Minecraft 美术审查员，检查二次编辑后的图是否满足：1）仍然只有一个独立素材，不是四宫格/合集/spritesheet；2）如需要透明/半透明，必须来自原生 alpha/provider 参数，而不是画出来的棋盘格、有色色块或提示词伪透明；3）优先检查 32x32 下是否轮廓清晰，简单/原版风素材再检查 16x16，只有高复杂度素材才建议 64x64/128x128；4）低噪声、主体居中、适合作为游戏贴图。确认后可调用 save_cached_image 保存该 edit_image 返回的 jobId。"},
            {"image", {
                {"mimeType", metadata["images"][0]["mimeType"]},
                {"base64", metadata["images"][0]["base64"]}
            }}
        };
    }

    metadata["workflow"] = {
        {"editedFromExistingImage", true},
        {"inMemoryReviewFirst", true},
        {"savedToFile", save_to_file},
        {"finalizeInstruction", "默认仅返回 base64 供 LLM 自审，并将编辑结果缓存在当前 MCP 进程内；确认素材完美后，调用 save_cached_image(jobId,index) 直接保存缓存图，避免重新编辑。"}
    };

    return tool_content_with_metadata("编辑完成：" + std::to_string(metadata["images"].size()) + " 张图像，默认以内存 base64 形式供自审，并已缓存可供 save_cached_image 直接保存。", metadata);
}

} // namespace

void register_image_tools(mcp::server& server, const AppConfig& config) {
    server.register_tool(build_generate_image_tool(), [config](const mcp::json& params, const std::string&) {
        return handle_generate_image(params, config);
    });

    server.register_tool(build_save_cached_image_tool(), [config](const mcp::json& params, const std::string&) {
        return handle_save_cached_image(params, config);
    });

    server.register_tool(build_process_image_tool(), [config](const mcp::json& params, const std::string&) {
        return handle_process_image(params, config);
    });

    server.register_tool(build_edit_image_tool(), [config](const mcp::json& params, const std::string&) {
        return handle_edit_image(params, config);
    });
}

} // namespace mcdk::image
