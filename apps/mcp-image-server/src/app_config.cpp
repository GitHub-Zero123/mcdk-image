#include "app_config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcdk::image {

std::optional<std::string> get_env_string(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    size_t required = 0;
    if (_dupenv_s(&raw, &required, name) != 0 || raw == nullptr) {
        return std::nullopt;
    }

    std::string value(raw);
    std::free(raw);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
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

namespace {

std::string env_or_default(const char* name, std::string fallback) {
    return get_env_string(name).value_or(std::move(fallback));
}

void apply_cli_arg(AppConfig& config, std::string_view key, std::string_view value) {
    if (key == "--output-dir") {
        config.output_dir = std::string(value);
    } else if (key == "--timeout-seconds") {
        config.timeout_seconds = parse_positive_int_or_default(std::string(value), config.timeout_seconds);
    } else if (key == "--log-level") {
        config.log_level = std::string(value);
    } else if (key == "--protocol" || key == "--base-url" || key == "--api-key" || key == "--api-key-env") {
        throw std::runtime_error("provider connection options must be passed by environment variables only");
    }
}

} // namespace

AppConfig load_app_config(int argc, char** argv) {
    AppConfig config;
    config.protocol = env_or_default("MCDK_IMAGE_PROTOCOL", "openai");
    config.base_url = env_or_default("MCDK_IMAGE_BASE_URL", "");
    config.api_key = env_or_default("MCDK_IMAGE_API_KEY", "");
    config.default_model = env_or_default("MCDK_IMAGE_DEFAULT_MODEL", "gpt-image-1");
    config.output_dir = env_or_default("MCDK_IMAGE_OUTPUT_DIR", "outputs");
    config.log_level = env_or_default("MCDK_IMAGE_LOG_LEVEL", "info");

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
    if (config.protocol != "openai") {
        throw std::runtime_error("unsupported MCDK_IMAGE_PROTOCOL: " + config.protocol);
    }
    if (config.base_url.empty()) {
        throw std::runtime_error("missing required environment variable: MCDK_IMAGE_BASE_URL");
    }
    if (config.api_key.empty()) {
        throw std::runtime_error("missing required environment variable: MCDK_IMAGE_API_KEY");
    }
}

} // namespace mcdk::image
