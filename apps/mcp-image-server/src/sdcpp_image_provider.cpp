#include "sdcpp_image_provider.hpp"

#include "image_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(MCDK_IMAGE_HAS_SDCPP)
#include "stable-diffusion.h"
#endif

namespace mcdk::image {

namespace {

std::pair<int, int> parse_size_or_default(const std::optional<std::string>& requested_size, const std::string& default_size) {
    const std::string value = requested_size && !requested_size->empty() ? *requested_size : default_size;
    const std::size_t x_pos = value.find('x');
    if (x_pos == std::string::npos) {
        return {512, 512};
    }

    try {
        const int width = std::stoi(value.substr(0, x_pos));
        const int height = std::stoi(value.substr(x_pos + 1));
        if (width > 0 && height > 0) {
            return {width, height};
        }
    } catch (...) {
    }
    return {512, 512};
}

int json_int_or(const Json& object, const char* key, int fallback) {
    if (!object.is_object() || !object.contains(key)) {
        return fallback;
    }
    if (object[key].is_number_integer()) {
        return object[key].get<int>();
    }
    if (object[key].is_number()) {
        return static_cast<int>(object[key].get<double>());
    }
    return fallback;
}

float json_float_or(const Json& object, const char* key, float fallback) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_number()) {
        return fallback;
    }
    return object[key].get<float>();
}

std::string json_string_or(const Json& object, const char* key, std::string fallback) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        return fallback;
    }
    return object[key].get<std::string>();
}

std::uint64_t make_random_seed() {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    return static_cast<std::uint64_t>(now) ^ (static_cast<std::uint64_t>(rd()) << 32U) ^ static_cast<std::uint64_t>(rd());
}

std::string path_for_metadata(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const std::filesystem::path normalized_path = std::filesystem::u8path(path).lexically_normal();
    const auto normalized = normalized_path.u8string();
    return std::string(reinterpret_cast<const char*>(normalized.data()), normalized.size());
}

#if defined(MCDK_IMAGE_HAS_SDCPP)

struct SdCtxDeleter {
    void operator()(sd_ctx_t* ctx) const noexcept {
        if (ctx) {
            free_sd_ctx(ctx);
        }
    }
};

struct SdImageArrayDeleter {
    int count = 0;

    void operator()(sd_image_t* images) const noexcept {
        if (!images) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            std::free(images[i].data);
        }
        std::free(images);
    }
};

using SdCtxPtr = std::unique_ptr<sd_ctx_t, SdCtxDeleter>;
using SdImageArrayPtr = std::unique_ptr<sd_image_t, SdImageArrayDeleter>;

std::vector<std::uint8_t> rgba_from_sd_image(const sd_image_t& image) {
    if (image.width == 0 || image.height == 0 || image.channel == 0 || image.data == nullptr) {
        throw std::runtime_error("sdcpp_error: generated an empty image");
    }

    const std::size_t pixel_count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    std::vector<std::uint8_t> rgba(pixel_count * 4U, 255);

    if (image.channel == 4) {
        std::copy_n(image.data, pixel_count * 4U, rgba.begin());
        return rgba;
    }

    if (image.channel == 3) {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            rgba[i * 4U + 0U] = image.data[i * 3U + 0U];
            rgba[i * 4U + 1U] = image.data[i * 3U + 1U];
            rgba[i * 4U + 2U] = image.data[i * 3U + 2U];
            rgba[i * 4U + 3U] = 255;
        }
        return rgba;
    }

    if (image.channel == 1) {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            rgba[i * 4U + 0U] = image.data[i];
            rgba[i * 4U + 1U] = image.data[i];
            rgba[i * 4U + 2U] = image.data[i];
            rgba[i * 4U + 3U] = 255;
        }
        return rgba;
    }

    throw std::runtime_error("sdcpp_error: unsupported image channel count: " + std::to_string(image.channel));
}

std::string normalize_protocol_name(std::string protocol) {
    std::transform(protocol.begin(), protocol.end(), protocol.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return protocol;
}

void apply_backend_preference(const StableDiffusionCppConfig& config) {
    const std::string preference = normalize_protocol_name(config.backend_preference);
    if (preference.find("cpu") != std::string::npos && preference.find("vulkan") == std::string::npos) {
#ifdef _WIN32
        _putenv_s("GGML_BACKEND", "CPU");
#else
        setenv("GGML_BACKEND", "CPU", 1);
#endif
    }
    // With SD_VULKAN enabled, stable-diffusion.cpp/ggml initializes the best GPU
    // device first and falls back to CPU when no Vulkan device can be initialized.
    // MCDK_IMAGE_SDCPP_BACKEND defaults to "vulkan,cpu" to document this runtime policy.
}

sd_ctx_params_t build_context_params(const AppConfig& app_config) {
    const StableDiffusionCppConfig& config = app_config.sdcpp;
    sd_ctx_params_t params;
    sd_ctx_params_init(&params);

    params.model_path = config.model_path.c_str();
    params.diffusion_model_path = config.diffusion_model_path.c_str();
    params.vae_path = config.vae_path.c_str();
    params.clip_l_path = config.clip_l_path.c_str();
    params.clip_g_path = config.clip_g_path.c_str();
    params.t5xxl_path = config.t5xxl_path.c_str();
    params.llm_path = config.llm_path.c_str();
    params.taesd_path = config.taesd_path.c_str();
    params.n_threads = config.threads > 0 ? config.threads : sd_get_num_physical_cores();
    params.keep_clip_on_cpu = config.keep_clip_on_cpu;
    params.keep_vae_on_cpu = config.keep_vae_on_cpu;
    params.offload_params_to_cpu = config.offload_params_to_cpu;
    params.enable_mmap = config.enable_mmap;
    params.rng_type = STD_DEFAULT_RNG;
    params.sampler_rng_type = STD_DEFAULT_RNG;

    return params;
}

sd_img_gen_params_t build_generation_params(const ImageGenerationRequest& request, const AppConfig& app_config, int width, int height, int batch_count) {
    const StableDiffusionCppConfig& config = app_config.sdcpp;
    sd_img_gen_params_t params;
    sd_img_gen_params_init(&params);

    const int steps = json_int_or(request.extra, "steps", config.steps);
    const float cfg_scale = json_float_or(request.extra, "cfgScale", config.cfg_scale);
    const std::string negative_prompt = json_string_or(request.extra, "negativePrompt", config.default_negative_prompt);
    const std::string sample_method_name = json_string_or(request.extra, "sampleMethod", config.sample_method);
    const std::string scheduler_name = json_string_or(request.extra, "scheduler", config.scheduler);

    params.prompt = request.prompt.c_str();
    params.negative_prompt = negative_prompt.c_str();
    params.width = width;
    params.height = height;
    params.batch_count = std::max(1, batch_count);
    params.seed = json_int_or(request.extra, "seed", config.seed);
    if (params.seed < 0) {
        params.seed = static_cast<int64_t>(make_random_seed() & static_cast<std::uint64_t>(std::numeric_limits<int32_t>::max()));
    }
    params.sample_params.sample_steps = std::max(1, steps);
    params.sample_params.guidance.txt_cfg = cfg_scale;
    params.sample_params.guidance.img_cfg = cfg_scale;

    if (!sample_method_name.empty()) {
        const sample_method_t parsed = str_to_sample_method(sample_method_name.c_str());
        if (parsed != SAMPLE_METHOD_COUNT) {
            params.sample_params.sample_method = parsed;
        }
    }
    if (!scheduler_name.empty()) {
        const scheduler_t parsed = str_to_scheduler(scheduler_name.c_str());
        if (parsed != SCHEDULER_COUNT) {
            params.sample_params.scheduler = parsed;
        }
    }

    return params;
}

#endif

} // namespace

StableDiffusionCppProvider::StableDiffusionCppProvider(AppConfig config)
    : config_(std::move(config)) {}

ImageGenerationResult StableDiffusionCppProvider::generate(const ImageGenerationRequest& request) {
#if !defined(MCDK_IMAGE_HAS_SDCPP)
    (void)request;
    throw std::runtime_error("sdcpp_error: stable-diffusion.cpp is not compiled in; initialize libs/stable-diffusion/ggml and reconfigure the project");
#else
    static std::mutex generation_mutex;
    std::lock_guard<std::mutex> lock(generation_mutex);

    apply_backend_preference(config_.sdcpp);

    const auto [width, height] = parse_size_or_default(request.size, config_.sdcpp.default_size);
    const int requested_n = request.n.value_or(1);
    const int batch_count = std::clamp(requested_n, 1, 4);

    sd_ctx_params_t context_params = build_context_params(config_);
    SdCtxPtr context(new_sd_ctx(&context_params));
    if (!context) {
        throw std::runtime_error("sdcpp_error: failed to initialize stable-diffusion.cpp context; check model paths and Vulkan/CPU backend availability");
    }
    if (!sd_ctx_supports_image_generation(context.get())) {
        throw std::runtime_error("sdcpp_error: configured model does not support image generation");
    }

    sd_img_gen_params_t generation_params = build_generation_params(request, config_, width, height, batch_count);
    if (generation_params.sample_params.sample_method == SAMPLE_METHOD_COUNT) {
        generation_params.sample_params.sample_method = sd_get_default_sample_method(context.get());
    }
    if (generation_params.sample_params.scheduler == SCHEDULER_COUNT) {
        generation_params.sample_params.scheduler = sd_get_default_scheduler(context.get(), generation_params.sample_params.sample_method);
    }

    sd_image_t* raw_images = generate_image(context.get(), &generation_params);
    if (!raw_images) {
        throw std::runtime_error("sdcpp_error: image generation failed");
    }
    SdImageArrayPtr images(raw_images, SdImageArrayDeleter{batch_count});

    ImageGenerationResult result;
    result.raw_response = {
        {"provider", "sdcpp"},
        {"modelPath", path_for_metadata(config_.sdcpp.model_path)},
        {"diffusionModelPath", path_for_metadata(config_.sdcpp.diffusion_model_path)},
        {"backendPreference", config_.sdcpp.backend_preference},
        {"systemInfo", sd_get_system_info()},
        {"width", width},
        {"height", height},
        {"steps", generation_params.sample_params.sample_steps},
        {"cfgScale", generation_params.sample_params.guidance.txt_cfg},
        {"seed", generation_params.seed},
        {"sampleMethod", sd_sample_method_name(generation_params.sample_params.sample_method)},
        {"scheduler", sd_scheduler_name(generation_params.sample_params.scheduler)}
    };

    for (int i = 0; i < batch_count; ++i) {
        ImageBuffer buffer;
        buffer.width = static_cast<int>(images.get()[i].width);
        buffer.height = static_cast<int>(images.get()[i].height);
        buffer.rgba = rgba_from_sd_image(images.get()[i]);

        ImageData image;
        image.mime_type = "image/png";
        image.bytes = encode_png(buffer);
        result.images.push_back(std::move(image));
    }

    return result;
#endif
}

} // namespace mcdk::image
