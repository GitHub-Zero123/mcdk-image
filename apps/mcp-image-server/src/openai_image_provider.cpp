#include "openai_image_provider.hpp"

#include "base64.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcdk::image {

namespace {

std::string trim_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::vector<std::uint8_t> bytes_from_string(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::string detect_mime_type(const std::vector<std::uint8_t>& bytes, const std::string& fallback = "image/png") {
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
        return "image/png";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
        return "image/jpeg";
    }
    if (bytes.size() >= 4 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F') {
        return "image/webp";
    }
    return fallback;
}

Json build_openai_payload(const ImageGenerationRequest& request) {
    Json payload = Json::object();
    payload["model"] = request.model;
    payload["prompt"] = request.prompt;
    if (request.n && *request.n > 0) {
        payload["n"] = *request.n;
    }
    if (request.size && !request.size->empty()) {
        payload["size"] = *request.size;
    }
    if (request.quality && !request.quality->empty()) {
        payload["quality"] = *request.quality;
    }
    if (request.style && !request.style->empty()) {
        payload["style"] = *request.style;
    }
    if (request.extra.is_object()) {
        for (const auto& item : request.extra.items()) {
            if (!payload.contains(item.key())) {
                payload[item.key()] = item.value();
            }
        }
    }
    return payload;
}

} // namespace

OpenAIImageProvider::OpenAIImageProvider(AppConfig config)
    : config_(std::move(config)) {}

std::string OpenAIImageProvider::generations_url() const {
    const std::string base = trim_trailing_slashes(config_.base_url);
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
        return base + "/images/generations";
    }
    return base + "/v1/images/generations";
}

ImageGenerationResult OpenAIImageProvider::generate(const ImageGenerationRequest& request) {
    HttpClient http(request.timeout_seconds > 0 ? request.timeout_seconds : config_.timeout_seconds);
    const Json payload = build_openai_payload(request);

    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + config_.api_key},
        {"Accept", "application/json"}
    };

    const HttpResponse response = http.post_json(generations_url(), headers, payload.dump());
    if (response.status == 0) {
        throw std::runtime_error("network_error: " + response.error);
    }
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("provider_error: HTTP " + std::to_string(response.status) + ": " + response.body);
    }

    Json parsed;
    try {
        parsed = Json::parse(response.body);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("provider_error: invalid JSON response: ") + e.what());
    }

    if (!parsed.contains("data") || !parsed["data"].is_array()) {
        throw std::runtime_error("provider_error: response missing data array");
    }

    ImageGenerationResult result;
    result.raw_response = parsed;

    for (const auto& item : parsed["data"]) {
        if (item.contains("b64_json") && item["b64_json"].is_string()) {
            const std::string decoded = base64::decode(item["b64_json"].get<std::string>());
            ImageData image;
            image.bytes = bytes_from_string(decoded);
            image.mime_type = detect_mime_type(image.bytes);
            result.images.push_back(std::move(image));
            continue;
        }

        if (item.contains("url") && item["url"].is_string()) {
            const HttpResponse image_response = http.get(item["url"].get<std::string>());
            if (image_response.status < 200 || image_response.status >= 300) {
                throw std::runtime_error("provider_error: failed to download image URL");
            }
            ImageData image;
            image.bytes = bytes_from_string(image_response.body);
            image.mime_type = detect_mime_type(image.bytes);
            result.images.push_back(std::move(image));
        }
    }

    if (result.images.empty()) {
        throw std::runtime_error("provider_error: no b64_json or url image data in response");
    }

    return result;
}

} // namespace mcdk::image
