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

#include <cstdlib>
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

namespace {

std::string env_or_default(const char* name, std::string fallback) {
    return get_env_string(name).value_or(std::move(fallback));
}

void apply_cli_arg(AppConfig& config, std::string_view key, std::string_view value) {
    if (key == "--timeout-seconds") {
        config.timeout_seconds = parse_positive_int_or_default(std::string(value), config.timeout_seconds);
    } else if (key == "--log-level") {
        config.log_level = std::string(value);
    } else if (key == "--output-dir" || key == "--protocol" || key == "--base-url" || key == "--api-key" || key == "--api-key-env") {
        throw std::runtime_error("file output paths and provider connection options must be passed by explicit tool arguments or environment variables as designed; --output-dir is forbidden");
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
