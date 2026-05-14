#pragma once

#include "openai_image_provider.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcdk::image {

struct ImageBuffer {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct ImageProcessingOptions {
    bool enabled = false;
    bool force_png_output = false;

    bool chroma_key_enabled = false;
    int chroma_key_r = 0;
    int chroma_key_g = 255;
    int chroma_key_b = 0;
    int chroma_key_tolerance = 72;
    int chroma_key_softness = 32;
    int chroma_key_spill_suppression = 48;

    bool nearest_resize_enabled = false;
    int nearest_target_width = 0;
    int nearest_target_height = 0;

    bool transparent_domain_scale_enabled = false;
    int transparent_padding = 0;
    int transparent_target_width = 0;
    int transparent_target_height = 0;
    int alpha_threshold = 1;

    bool pixel_art_compress_enabled = false;
    int pixel_art_max_width = 0;
    int pixel_art_max_height = 0;

    bool remove_fake_transparency_enabled = false;
    int fake_transparency_tolerance = 16;
    int fake_transparency_alpha = 0;
};

ImageProcessingOptions parse_processing_options(const Json& params);
ImageBuffer decode_image_rgba(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_png(const ImageBuffer& image);
ImageBuffer nearest_resize(const ImageBuffer& source, int target_width, int target_height);
ImageBuffer transparent_domain_scale(const ImageBuffer& source, int target_width, int target_height, int padding, int alpha_threshold);
ImageBuffer remove_fake_transparency_background(const ImageBuffer& source, int tolerance, int target_alpha);
ImageBuffer remove_chroma_key_background(const ImageBuffer& source, int key_r, int key_g, int key_b, int tolerance, int softness, int spill_suppression);
ImageData process_image_data(const ImageData& input, const ImageProcessingOptions& options);

std::string base64_encode_bytes(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> base64_decode_bytes(const std::string& encoded);
std::vector<std::uint8_t> read_binary_file(const std::string& path);
void write_binary_file(const std::string& path, const std::vector<std::uint8_t>& bytes);

} // namespace mcdk::image
