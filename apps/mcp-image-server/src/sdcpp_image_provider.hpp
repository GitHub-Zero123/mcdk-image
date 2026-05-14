#pragma once

#include "openai_image_provider.hpp"

namespace mcdk::image {

class StableDiffusionCppProvider final : public ImageProvider {
public:
    explicit StableDiffusionCppProvider(AppConfig config);

    ImageGenerationResult generate(const ImageGenerationRequest& request) override;

private:
    AppConfig config_;
};

} // namespace mcdk::image
