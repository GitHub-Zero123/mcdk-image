#include "mcp_tools.hpp"

#include "image_processor.hpp"
#include "openai_image_provider.hpp"

#include "mcp_message.h"
#include "mcp_tool.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mcdk::image {

namespace {

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
    request.size = string_param(params, "size");
    request.quality = string_param(params, "quality");
    request.style = string_param(params, "style");
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
        {"nearestResize", {{"type", "object"}, {"description", "Nearest-neighbor resize options"}}},
        {"transparentDomainScale", {{"type", "object"}, {"description", "Crop transparent domain and resize options"}}},
        {"pixelArtCompress", {{"type", "object"}, {"description", "Pixel-art compression options"}}}
    };

    return mcp::tool_builder("generate_image")
        .with_description("Generate an image through the environment-configured OpenAI-compatible image provider, optionally post-process it for Minecraft pixel art, and save/return base64 data.")
        .with_string_param("prompt", "Image prompt", true)
        .with_string_param("model", "Image model override for this request", false)
        .with_string_param("size", "Provider image size such as 1024x1024", false)
        .with_number_param("n", "Number of images, 1-4", false)
        .with_string_param("quality", "Provider quality option", false)
        .with_string_param("style", "Provider style option", false)
        .with_object_param("extra", "Extra provider JSON fields, excluding connection config", Json::object(), false)
        .with_object_param("processing", "Optional image processing pipeline", processing_properties, false)
        .with_boolean_param("returnBase64", "Return image base64 in metadata", false)
        .with_boolean_param("saveToFile", "Save image files to output directory", false)
        .with_boolean_param("selfReviewHint", "Return a visual self-review prompt and base64 payload", false)
        .with_number_param("timeoutSeconds", "Per-request timeout seconds, default 400", false)
        .with_open_world_hint(true)
        .build();
}

mcp::tool build_process_image_tool() {
    Json processing_properties = {
        {"enabled", {{"type", "boolean"}, {"description", "Enable post-processing pipeline"}}},
        {"nearestResize", {{"type", "object"}, {"description", "Nearest-neighbor resize options"}}},
        {"transparentDomainScale", {{"type", "object"}, {"description", "Crop transparent domain and resize options"}}},
        {"pixelArtCompress", {{"type", "object"}, {"description", "Pixel-art compression options"}}}
    };

    return mcp::tool_builder("process_image")
        .with_description("Process a local image file or base64 image with Minecraft-oriented nearest-neighbor and transparent-domain operations.")
        .with_string_param("inputPath", "Input image file path", false)
        .with_string_param("inputBase64", "Input image base64", false)
        .with_string_param("outputPath", "Output file path", false)
        .with_object_param("processing", "Image processing pipeline", processing_properties, true)
        .with_boolean_param("returnBase64", "Return output image base64", false)
        .with_read_only_hint(false)
        .build();
}

Json handle_generate_image(const Json& params, const AppConfig& config) {
    const auto started = std::chrono::steady_clock::now();
    const std::string job_id = make_job_id();
    const bool return_base64 = bool_param(params, "returnBase64", false);
    const bool save_to_file = bool_param(params, "saveToFile", true);
    const bool self_review_hint = bool_param(params, "selfReviewHint", false);

    OpenAIImageProvider provider(config);
    ImageGenerationRequest request = parse_generation_request(params, config);
    ImageProcessingOptions processing = parse_processing_options(params);
    ImageGenerationResult generated = provider.generate(request);

    Json metadata = Json::object();
    metadata["jobId"] = job_id;
    metadata["provider"] = config.protocol;
    metadata["model"] = request.model;
    metadata["images"] = Json::array();

    std::size_t index = 0;
    for (const ImageData& source_image : generated.images) {
        ImageData image = process_image_data(source_image, processing);
        Json image_meta = Json::object();
        image_meta["index"] = index;
        image_meta["mimeType"] = image.mime_type;
        image_meta["bytes"] = image.bytes.size();

        if (save_to_file) {
            std::filesystem::path output_path = std::filesystem::path(config.output_dir) /
                ("mcdk-image-" + job_id + "-" + std::to_string(index) + extension_from_mime(image.mime_type));
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
            {"pixelArtCompressApplied", processing.enabled && processing.pixel_art_compress_enabled}
        };
        metadata["images"].push_back(std::move(image_meta));
        ++index;
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    metadata["elapsedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (self_review_hint && !metadata["images"].empty() && metadata["images"][0].contains("base64")) {
        metadata["selfReview"] = {
            {"enabled", true},
            {"prompt", "请作为 Minecraft 美术审查员，检查该图是否满足：清晰轮廓、透明背景、低色彩噪声、主体居中、适合贴图/像素画。若不满足，请输出下一轮改进 prompt。"},
            {"image", {
                {"mimeType", metadata["images"][0]["mimeType"]},
                {"base64", metadata["images"][0]["base64"]}
            }}
        };
    }

    return tool_content_with_metadata("生成完成：" + std::to_string(metadata["images"].size()) + " 张图像。", metadata);
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

    server.register_tool(build_process_image_tool(), [config](const mcp::json& params, const std::string&) {
        return handle_process_image(params, config);
    });
}

} // namespace mcdk::image
