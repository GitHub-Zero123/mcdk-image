#pragma once

#include "app_config.hpp"
#include "http_client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcdk::image {

using Json = nlohmann::ordered_json;

struct ImageData {
    std::string mime_type = "image/png";
    std::vector<std::uint8_t> bytes;
};

struct ImageGenerationRequest {
    std::string prompt;
    std::string model;
    std::optional<std::string> size;
    std::optional<int> n;
    std::optional<std::string> quality;
    std::optional<std::string> style;
    Json extra = Json::object();
    int timeout_seconds = 400;
};

struct ImageGenerationResult {
    std::vector<ImageData> images;
    Json raw_response = Json::object();
};

class ImageProvider {
public:
    virtual ~ImageProvider() = default;
    virtual ImageGenerationResult generate(const ImageGenerationRequest& request) = 0;
};

class OpenAIImageProvider final : public ImageProvider {
public:
    explicit OpenAIImageProvider(AppConfig config);

    ImageGenerationResult generate(const ImageGenerationRequest& request) override;

private:
    AppConfig config_;

    std::string generations_url() const;
};

} // namespace mcdk::image
