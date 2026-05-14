#include "app_config.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcdk::image {

#ifdef _WIN32
std::wstring ascii_to_wide(std::string_view value) {
    std::wstring wide;
    wide.reserve(value.size());
    for (const char ch : value) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return wide;
}

std::string wide_to_utf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8.data(), required, nullptr, nullptr);
    return utf8;
}
#endif

std::optional<std::string> get_env_string(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name = ascii_to_wide(name);
    size_t required = 0;
    if (_wgetenv_s(&required, nullptr, 0, wide_name.c_str()) != 0 || required == 0) {
        return std::nullopt;
    }

    std::wstring value(required, L'\0');
    if (_wgetenv_s(&required, value.data(), value.size(), wide_name.c_str()) != 0 || required <= 1) {
        return std::nullopt;
    }
    value.resize(required - 1);

    std::string utf8 = wide_to_utf8(value.c_str());
    if (utf8.empty()) {
        return std::nullopt;
    }
    return utf8;
#else
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

int parse_positive_int_or_default(const std::string& value, int fallback) {
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

float parse_positive_float_or_default(const std::string& value, float fallback) {
    try {
        const float parsed = std::stof(value);
        return parsed > 0.0f ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

bool parse_bool_or_default(const std::string& value, bool fallback) {
    std::string normalized;
    normalized.reserve(value.size());
    std::transform(value.begin(), value.end(), std::back_inserter(normalized), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return fallback;
}

namespace {

std::string env_or_default(const char* name, std::string fallback) {
    return get_env_string(name).value_or(std::move(fallback));
}

bool has_any_sdcpp_model_path(const StableDiffusionCppConfig& config) {
    return !config.model_path.empty()
        || !config.diffusion_model_path.empty();
}

void apply_cli_arg(AppConfig& config, std::string_view key, std::string_view value) {
    if (key == "--timeout-seconds") {
        config.timeout_seconds = parse_positive_int_or_default(std::string(value), config.timeout_seconds);
    } else if (key == "--log-level") {
        config.log_level = std::string(value);
    } else if (key == "--output-dir" || key == "--protocol" || key == "--base-url" || key == "--api-key" || key == "--api-key-env" || key == "--model" || key == "--model-path") {
        throw std::runtime_error("file output paths and provider connection/model options must be passed by explicit tool arguments or environment variables as designed; provider/model CLI args are forbidden");
    }
}

void load_sdcpp_config(AppConfig& config) {
    config.sdcpp.model_path = env_or_default("MCDK_IMAGE_SDCPP_MODEL_PATH", "");
    config.sdcpp.diffusion_model_path = env_or_default("MCDK_IMAGE_SDCPP_DIFFUSION_MODEL_PATH", "");
    config.sdcpp.vae_path = env_or_default("MCDK_IMAGE_SDCPP_VAE_PATH", "");
    config.sdcpp.clip_l_path = env_or_default("MCDK_IMAGE_SDCPP_CLIP_L_PATH", "");
    config.sdcpp.clip_g_path = env_or_default("MCDK_IMAGE_SDCPP_CLIP_G_PATH", "");
    config.sdcpp.t5xxl_path = env_or_default("MCDK_IMAGE_SDCPP_T5XXL_PATH", "");
    config.sdcpp.llm_path = env_or_default("MCDK_IMAGE_SDCPP_LLM_PATH", "");
    config.sdcpp.taesd_path = env_or_default("MCDK_IMAGE_SDCPP_TAESD_PATH", "");
    config.sdcpp.backend_preference = env_or_default("MCDK_IMAGE_SDCPP_BACKEND", "vulkan,cpu");
    config.sdcpp.default_size = env_or_default("MCDK_IMAGE_SDCPP_DEFAULT_SIZE", "512x512");
    config.sdcpp.default_negative_prompt = env_or_default("MCDK_IMAGE_SDCPP_NEGATIVE_PROMPT", "text, watermark, logo, signature, blurry, noisy, multiple objects, grid, spritesheet, checkerboard background");
    config.sdcpp.sample_method = env_or_default("MCDK_IMAGE_SDCPP_SAMPLE_METHOD", "");
    config.sdcpp.scheduler = env_or_default("MCDK_IMAGE_SDCPP_SCHEDULER", "");

    if (const auto steps = get_env_string("MCDK_IMAGE_SDCPP_STEPS")) {
        config.sdcpp.steps = parse_positive_int_or_default(*steps, config.sdcpp.steps);
    }
    if (const auto cfg = get_env_string("MCDK_IMAGE_SDCPP_CFG_SCALE")) {
        config.sdcpp.cfg_scale = parse_positive_float_or_default(*cfg, config.sdcpp.cfg_scale);
    }
    if (const auto seed = get_env_string("MCDK_IMAGE_SDCPP_SEED")) {
        try {
            config.sdcpp.seed = std::stoi(*seed);
        } catch (...) {
            config.sdcpp.seed = -1;
        }
    }
    if (const auto threads = get_env_string("MCDK_IMAGE_SDCPP_THREADS")) {
        config.sdcpp.threads = parse_positive_int_or_default(*threads, config.sdcpp.threads);
    }
    if (const auto keep_clip = get_env_string("MCDK_IMAGE_SDCPP_KEEP_CLIP_ON_CPU")) {
        config.sdcpp.keep_clip_on_cpu = parse_bool_or_default(*keep_clip, config.sdcpp.keep_clip_on_cpu);
    }
    if (const auto keep_vae = get_env_string("MCDK_IMAGE_SDCPP_KEEP_VAE_ON_CPU")) {
        config.sdcpp.keep_vae_on_cpu = parse_bool_or_default(*keep_vae, config.sdcpp.keep_vae_on_cpu);
    }
    if (const auto offload = get_env_string("MCDK_IMAGE_SDCPP_OFFLOAD_PARAMS_TO_CPU")) {
        config.sdcpp.offload_params_to_cpu = parse_bool_or_default(*offload, config.sdcpp.offload_params_to_cpu);
    }
    if (const auto mmap = get_env_string("MCDK_IMAGE_SDCPP_ENABLE_MMAP")) {
        config.sdcpp.enable_mmap = parse_bool_or_default(*mmap, config.sdcpp.enable_mmap);
    }
}

} // namespace

AppConfig load_app_config(int argc, char** argv) {
    AppConfig config;
    config.protocol = env_or_default("MCDK_IMAGE_PROTOCOL", "openai");
    config.base_url = env_or_default("MCDK_IMAGE_BASE_URL", "");
    config.api_key = env_or_default("MCDK_IMAGE_API_KEY", "");
    config.default_model = env_or_default("MCDK_IMAGE_DEFAULT_MODEL", "gpt-image-2");
    config.log_level = env_or_default("MCDK_IMAGE_LOG_LEVEL", "info");
    load_sdcpp_config(config);

    if (const auto timeout = get_env_string("MCDK_IMAGE_TIMEOUT_SECONDS")) {
        config.timeout_seconds = parse_positive_int_or_default(*timeout, config.timeout_seconds);
    }

    for (int i = 1; i + 1 < argc; i += 2) {
        apply_cli_arg(config, argv[i], argv[i + 1]);
    }

    validate_provider_config(config);
    return config;
}

void validate_provider_config(const AppConfig& config) {
    if (config.protocol == "openai") {
        if (config.base_url.empty()) {
            throw std::runtime_error("missing required environment variable: MCDK_IMAGE_BASE_URL");
        }
        if (config.api_key.empty()) {
            throw std::runtime_error("missing required environment variable: MCDK_IMAGE_API_KEY");
        }
        return;
    }

    if (config.protocol == "sdcpp" || config.protocol == "stable-diffusion.cpp") {
        if (!has_any_sdcpp_model_path(config.sdcpp)) {
            throw std::runtime_error("missing required environment variable: MCDK_IMAGE_SDCPP_MODEL_PATH or MCDK_IMAGE_SDCPP_DIFFUSION_MODEL_PATH");
        }
        return;
    }

    throw std::runtime_error("unsupported MCDK_IMAGE_PROTOCOL: " + config.protocol);
}

} // namespace mcdk::image
