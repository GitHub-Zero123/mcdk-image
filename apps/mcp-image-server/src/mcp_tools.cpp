#include "mcp_tools.hpp"

#include "image_processor.hpp"
#include "openai_image_provider.hpp"

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

std::string extension_from_mime(const std::string& mime) {
    if (mime == "image/jpeg") {
        return ".jpg";
    }
    if (mime == "image/webp") {
        return ".webp";
    }
    return ".png";
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

ImageGenerationRequest parse_generation_request(const Json& params, const AppConfig& config) {
    if (!params.contains("prompt") || !params["prompt"].is_string() || params["prompt"].get<std::string>().empty()) {
        throw mcp::mcp_exception(mcp::error_code::invalid_params, "prompt is required");
    }

    ImageGenerationRequest request;
    request.prompt = params["prompt"].get<std::string>();
    request.model = string_param(params, "model").value_or(config.default_model);
    request.size = string_param(params, "size").value_or("1024x1024");
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

mcp::tool build_generate_image_tool() {
    Json processing_properties = {
        {"enabled", {{"type", "boolean"}, {"description", "Enable post-processing pipeline"}}},
        {"nearestResize", {{"type", "object"}, {"description", "Nearest-neighbor resize options. For Minecraft pixel art, prefer 16x16 or 32x32; 32x32 is usually the most balanced. Use 64x64/128x128 only for high-complexity assets."}}},
        {"transparentDomainScale", {{"type", "object"}, {"description", "Crop transparent domain and resize options"}}},
        {"pixelArtCompress", {{"type", "object"}, {"description", "Pixel-art compression options. Prefer 16x16 or 32x32 for most Minecraft assets; 32x32 is the most balanced. Use 64x64/128x128 only when detail complexity requires it."}}},
        {"removeFakeTransparency", {{"type", "object"}, {"description", "Convert fake checkerboard/solid background colors into real alpha transparency"}}}
    };

    return mcp::tool_builder("generate_image")
        .with_description("Generate ONE Minecraft game asset per image through the environment-configured OpenAI-compatible image provider. Default generation size is 1024x1024 because some gateways reject 512x512 with upstream errors; post-process down to 32x32 or 16x16 for most Minecraft pixel-art assets. 32x32 is usually the most balanced. Use 64x64/128x128 only for high-complexity assets. Default workflow keeps the image in memory and returns base64 for LLM self-review; only save to file when saveToFile=true after review. Prompts should request a single item/entity/block asset, not a grid, collage, spritesheet, or multiple objects. True transparency/semi-transparency should be requested through nativeTransparency or provider extra fields, not only through prompt wording.")
        .with_string_param("prompt", "Image prompt", true)
        .with_string_param("model", "Image model override for this request", false)
        .with_string_param("size", "Optional provider image size. Defaults to 1024x1024 because some gateways reject 512x512; use smaller sizes only when the provider is known to support them.", false)
        .with_number_param("n", "Number of images, 1-4", false)
        .with_string_param("quality", "Provider quality option", false)
        .with_string_param("style", "Provider style option", false)
        .with_boolean_param("nativeTransparency", "Request native transparent PNG output through provider parameters. Do not rely on prompt wording alone for transparency/semi-transparency.", false)
        .with_object_param("extra", "Extra provider JSON fields, excluding connection config. Use this for provider-specific native alpha/transparency controls.", Json::object(), false)
        .with_object_param("processing", "Optional image processing pipeline", processing_properties, false)
        .with_boolean_param("returnBase64", "Return image base64 in metadata. Defaults to true for in-memory review.", false)
        .with_boolean_param("saveToFile", "Save image files to output directory. Defaults to false; enable only after self-review accepts the asset.", false)
        .with_string_param("outputPath", "Optional output file path/name when saveToFile=true. For multiple images, an index is appended before the extension.", false)
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
        .with_string_param("outputPath", "Optional output file path. Defaults to configured output directory", false)
        .with_read_only_hint(false)
        .build();
}

mcp::tool build_process_image_tool() {
    Json processing_properties = {
        {"enabled", {{"type", "boolean"}, {"description", "Enable post-processing pipeline"}}},
        {"nearestResize", {{"type", "object"}, {"description", "Nearest-neighbor resize options. For Minecraft pixel art, prefer 16x16 or 32x32; 32x32 is usually the most balanced. Use 64x64/128x128 only for high-complexity assets."}}},
        {"transparentDomainScale", {{"type", "object"}, {"description", "Crop transparent domain and resize options"}}},
        {"pixelArtCompress", {{"type", "object"}, {"description", "Pixel-art compression options. Prefer 16x16 or 32x32 for most Minecraft assets; 32x32 is the most balanced. Use 64x64/128x128 only when detail complexity requires it."}}},
        {"removeFakeTransparency", {{"type", "object"}, {"description", "Convert fake checkerboard/solid background colors into real alpha transparency"}}}
    };

    return mcp::tool_builder("process_image")
        .with_description("Process a local image file or base64 image with Minecraft-oriented nearest-neighbor, transparent-domain, 16/32/64/128 pixel-art sizing, and fake-transparency cleanup operations.")
        .with_string_param("inputPath", "Input image file path", false)
        .with_string_param("inputBase64", "Input image base64", false)
        .with_string_param("outputPath", "Output file path", false)
        .with_object_param("processing", "Image processing pipeline", processing_properties, true)
        .with_boolean_param("returnBase64", "Return output image base64", false)
        .with_read_only_hint(false)
        .build();
}

std::string minecraft_asset_prompt(const std::string& user_prompt) {
    return user_prompt
        + "\n\nStrict Minecraft asset rules: generate exactly ONE standalone game asset in this image. "
          "Do not create a spritesheet, grid, collage, collection, multiple items, hands, characters, labels, text, watermark, or UI. "
          "Use a clean source composition that can be downsampled to Minecraft texture sizes; prefer 32x32 for balanced item textures, 16x16 for vanilla-like/simple assets, and reserve 64x64/128x128 for high-complexity assets only. "
          "Do not draw checkerboard transparency, grey grid backgrounds, colored blocks, or fake transparent tiles. "
          "Do not describe semi-transparency as a visual prompt-only requirement; native alpha/semi-transparency must be requested through tool/provider parameters. "
          "Keep a clean readable silhouette, centered object, crisp pixel-art edges, and low-noise colors.";
}

Json handle_generate_image(const Json& params, const AppConfig& config) {
    const auto started = std::chrono::steady_clock::now();
    const std::string job_id = make_job_id();
    const bool return_base64 = bool_param(params, "returnBase64", true);
    const bool save_to_file = bool_param(params, "saveToFile", false);
    const bool self_review_hint = bool_param(params, "selfReviewHint", true);

    OpenAIImageProvider provider(config);
    ImageGenerationRequest request = parse_generation_request(params, config);
    request.prompt = minecraft_asset_prompt(request.prompt);
    ImageProcessingOptions processing = parse_processing_options(params);
    ImageGenerationResult generated = provider.generate(request);

    Json metadata = Json::object();
    metadata["jobId"] = job_id;
    metadata["provider"] = config.protocol;
    metadata["model"] = request.model;
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
            std::filesystem::path output_path;
            if (const auto requested_output = string_param(params, "outputPath")) {
                output_path = std::filesystem::path(*requested_output);
                if (generated.images.size() > 1) {
                    const std::filesystem::path parent = output_path.parent_path();
                    const std::string stem = output_path.stem().string();
                    const std::string ext = output_path.has_extension() ? output_path.extension().string() : extension_from_mime(image.mime_type);
                    output_path = parent / (stem + "-" + std::to_string(index) + ext);
                }
            } else {
                output_path = std::filesystem::path(config.output_dir) /
                    ("mcdk-image-" + job_id + "-" + std::to_string(index) + extension_from_mime(image.mime_type));
            }
            write_binary_file(output_path.string(), image.bytes);
            image_meta["filePath"] = output_path.generic_string();
        }

        if (return_base64 || self_review_hint) {
            image_meta["base64"] = base64_encode_bytes(image.bytes);
        }

        image_meta["processing"] = {
            {"enabled", processing.enabled},
            {"nearestResizeApplied", processing.enabled && processing.nearest_resize_enabled},
            {"transparentDomainScaleApplied", processing.enabled && processing.transparent_domain_scale_enabled},
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

    CachedImage cached;
    {
        std::lock_guard<std::mutex> lock(image_cache_mutex());
        auto job_it = image_cache().find(*job_id);
        if (job_it == image_cache().end()) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "cached jobId not found or MCP process was restarted");
        }
        if (static_cast<std::size_t>(image_index) >= job_it->second.size()) {
            throw mcp::mcp_exception(mcp::error_code::invalid_params, "cached image index out of range");
        }
        cached = job_it->second[static_cast<std::size_t>(image_index)];
    }

    const std::string output_path = string_param(params, "outputPath").value_or(
        (std::filesystem::path(config.output_dir) / ("mcdk-image-" + *job_id + "-" + std::to_string(image_index) + extension_from_mime(cached.mime_type))).string()
    );
    write_binary_file(output_path, cached.bytes);

    Json metadata = {
        {"jobId", *job_id},
        {"index", image_index},
        {"mimeType", cached.mime_type},
        {"bytes", cached.bytes.size()},
        {"filePath", std::filesystem::path(output_path).generic_string()},
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
    input.bytes = input_path ? read_binary_file(*input_path) : base64_decode_bytes(*input_base64);

    ImageProcessingOptions processing = parse_processing_options(params);
    processing.enabled = true;
    ImageData output = process_image_data(input, processing);

    Json metadata = Json::object();
    metadata["mimeType"] = output.mime_type;
    metadata["bytes"] = output.bytes.size();

    const std::string output_path = string_param(params, "outputPath").value_or(
        (std::filesystem::path(config.output_dir) / ("processed-" + make_job_id() + ".png")).string()
    );
    write_binary_file(output_path, output.bytes);
    metadata["filePath"] = std::filesystem::path(output_path).generic_string();

    if (return_base64) {
        metadata["base64"] = base64_encode_bytes(output.bytes);
    }

    return tool_content_with_metadata("图像处理完成。", metadata);
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
}

} // namespace mcdk::image
