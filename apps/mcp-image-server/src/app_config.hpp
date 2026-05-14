#pragma once

#include <optional>
#include <string>

namespace mcdk::image {

struct StableDiffusionCppConfig {
    std::string model_path;
    std::string diffusion_model_path;
    std::string vae_path;
    std::string clip_l_path;
    std::string clip_g_path;
    std::string t5xxl_path;
    std::string llm_path;
    std::string taesd_path;
    std::string backend_preference = "vulkan,cpu";
    std::string default_size = "512x512";
    std::string default_negative_prompt;
    std::string sample_method;
    std::string scheduler;
    int steps = 20;
    float cfg_scale = 7.0f;
    int seed = -1;
    int threads = 0;
    bool keep_clip_on_cpu = false;
    bool keep_vae_on_cpu = false;
    bool offload_params_to_cpu = false;
    bool enable_mmap = false;
};

struct AppConfig {
    std::string protocol;
    std::string base_url;
    std::string api_key;
    std::string default_model;
    int timeout_seconds = 400;
    std::string log_level;
    StableDiffusionCppConfig sdcpp;
};

AppConfig load_app_config(int argc, char** argv);
void validate_provider_config(const AppConfig& config);

std::optional<std::string> get_env_string(const char* name);
int parse_positive_int_or_default(const std::string& value, int fallback);
float parse_positive_float_or_default(const std::string& value, float fallback);
bool parse_bool_or_default(const std::string& value, bool fallback);

} // namespace mcdk::image
