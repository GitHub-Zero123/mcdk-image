#pragma once

#include <optional>
#include <string>

namespace mcdk::image {

struct AppConfig {
    std::string protocol;
    std::string base_url;
    std::string api_key;
    std::string default_model;
    std::string output_dir;
    int timeout_seconds = 400;
    std::string log_level;
};

AppConfig load_app_config(int argc, char** argv);
void validate_provider_config(const AppConfig& config);

std::optional<std::string> get_env_string(const char* name);
int parse_positive_int_or_default(const std::string& value, int fallback);

} // namespace mcdk::image
