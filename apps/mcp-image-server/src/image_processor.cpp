#include "image_processor.hpp"

#include "base64.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace mcdk::image {

namespace {

int json_int_or(const Json& object, const char* key, int fallback) {
    if (object.contains(key) && object[key].is_number_integer()) {
        return object[key].get<int>();
    }
    if (object.contains(key) && object[key].is_number()) {
        return static_cast<int>(object[key].get<double>());
    }
    return fallback;
}

bool json_bool_or(const Json& object, const char* key, bool fallback) {
    return object.contains(key) && object[key].is_boolean() ? object[key].get<bool>() : fallback;
}

struct PngWriteContext {
    std::vector<std::uint8_t> bytes;
};

void png_write_callback(void* context, void* data, int size) {
    auto* output = static_cast<PngWriteContext*>(context);
    const auto* begin = static_cast<const std::uint8_t*>(data);
    output->bytes.insert(output->bytes.end(), begin, begin + size);
}

ImageBuffer crop_rgba(const ImageBuffer& source, int left, int top, int width, int height) {
    ImageBuffer cropped;
    cropped.width = width;
    cropped.height = height;
    cropped.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(top + y) * source.width + (left + x)) * 4;
            const std::size_t dst = (static_cast<std::size_t>(y) * width + x) * 4;
            std::copy_n(source.rgba.begin() + static_cast<std::ptrdiff_t>(src), 4, cropped.rgba.begin() + static_cast<std::ptrdiff_t>(dst));
        }
    }
    return cropped;
}

} // namespace

ImageProcessingOptions parse_processing_options(const Json& params) {
    ImageProcessingOptions options;
    if (!params.contains("processing") || !params["processing"].is_object()) {
        return options;
    }

    const Json& processing = params["processing"];
    options.enabled = json_bool_or(processing, "enabled", false);

    if (processing.contains("nearestResize") && processing["nearestResize"].is_object()) {
        const Json& nearest = processing["nearestResize"];
        options.nearest_resize_enabled = json_bool_or(nearest, "enabled", false);
        options.nearest_target_width = json_int_or(nearest, "targetWidth", 0);
        options.nearest_target_height = json_int_or(nearest, "targetHeight", 0);
    }

    if (processing.contains("transparentDomainScale") && processing["transparentDomainScale"].is_object()) {
        const Json& transparent = processing["transparentDomainScale"];
        options.transparent_domain_scale_enabled = json_bool_or(transparent, "enabled", false);
        options.transparent_padding = json_int_or(transparent, "padding", 0);
        options.transparent_target_width = json_int_or(transparent, "targetWidth", 0);
        options.transparent_target_height = json_int_or(transparent, "targetHeight", 0);
        options.alpha_threshold = json_int_or(transparent, "alphaThreshold", 1);
    }

    if (processing.contains("pixelArtCompress") && processing["pixelArtCompress"].is_object()) {
        const Json& pixel = processing["pixelArtCompress"];
        options.pixel_art_compress_enabled = json_bool_or(pixel, "enabled", false);
        options.pixel_art_max_width = json_int_or(pixel, "maxWidth", 0);
        options.pixel_art_max_height = json_int_or(pixel, "maxHeight", 0);
    }

    return options;
}

ImageBuffer decode_image_rgba(const std::vector<std::uint8_t>& bytes) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0) {
        throw std::runtime_error("image_decode_error: failed to decode image data");
    }

    ImageBuffer image;
    image.width = width;
    image.height = height;
    image.rgba.assign(decoded, decoded + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(decoded);
    return image;
}

std::vector<std::uint8_t> encode_png(const ImageBuffer& image) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        throw std::runtime_error("image_encode_error: invalid image buffer");
    }

    PngWriteContext context;
    const int ok = stbi_write_png_to_func(
        png_write_callback,
        &context,
        image.width,
        image.height,
        4,
        image.rgba.data(),
        image.width * 4
    );
    if (!ok) {
        throw std::runtime_error("image_encode_error: failed to encode PNG");
    }
    return context.bytes;
}

ImageBuffer nearest_resize(const ImageBuffer& source, int target_width, int target_height) {
    if (target_width <= 0 || target_height <= 0) {
        throw std::runtime_error("image_process_error: target size must be positive");
    }

    ImageBuffer output;
    output.width = target_width;
    output.height = target_height;
    output.rgba.resize(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height) * 4);

    for (int y = 0; y < target_height; ++y) {
        const int src_y = std::min(source.height - 1, (y * source.height) / target_height);
        for (int x = 0; x < target_width; ++x) {
            const int src_x = std::min(source.width - 1, (x * source.width) / target_width);
            const std::size_t src = (static_cast<std::size_t>(src_y) * source.width + src_x) * 4;
            const std::size_t dst = (static_cast<std::size_t>(y) * target_width + x) * 4;
            std::copy_n(source.rgba.begin() + static_cast<std::ptrdiff_t>(src), 4, output.rgba.begin() + static_cast<std::ptrdiff_t>(dst));
        }
    }
    return output;
}

ImageBuffer transparent_domain_scale(const ImageBuffer& source, int target_width, int target_height, int padding, int alpha_threshold) {
    if (target_width <= 0 || target_height <= 0) {
        throw std::runtime_error("image_process_error: transparentDomainScale target size must be positive");
    }

    int left = source.width;
    int top = source.height;
    int right = -1;
    int bottom = -1;
    const int threshold = std::clamp(alpha_threshold, 0, 255);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * source.width + x) * 4 + 3;
            if (source.rgba[index] > threshold) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }

    if (right < left || bottom < top) {
        return nearest_resize(source, target_width, target_height);
    }

    const int pad = std::max(0, padding);
    left = std::max(0, left - pad);
    top = std::max(0, top - pad);
    right = std::min(source.width - 1, right + pad);
    bottom = std::min(source.height - 1, bottom + pad);

    ImageBuffer cropped = crop_rgba(source, left, top, right - left + 1, bottom - top + 1);
    return nearest_resize(cropped, target_width, target_height);
}

ImageData process_image_data(const ImageData& input, const ImageProcessingOptions& options) {
    if (!options.enabled) {
        return input;
    }

    ImageBuffer image = decode_image_rgba(input.bytes);

    if (options.transparent_domain_scale_enabled) {
        image = transparent_domain_scale(
            image,
            options.transparent_target_width,
            options.transparent_target_height,
            options.transparent_padding,
            options.alpha_threshold
        );
    }

    if (options.nearest_resize_enabled) {
        image = nearest_resize(image, options.nearest_target_width, options.nearest_target_height);
    }

    if (options.pixel_art_compress_enabled && options.pixel_art_max_width > 0 && options.pixel_art_max_height > 0) {
        const double scale_x = static_cast<double>(options.pixel_art_max_width) / static_cast<double>(image.width);
        const double scale_y = static_cast<double>(options.pixel_art_max_height) / static_cast<double>(image.height);
        const double scale = std::min(1.0, std::min(scale_x, scale_y));
        const int target_width = std::max(1, static_cast<int>(image.width * scale));
        const int target_height = std::max(1, static_cast<int>(image.height * scale));
        if (target_width != image.width || target_height != image.height) {
            image = nearest_resize(image, target_width, target_height);
        }
    }

    ImageData output;
    output.mime_type = "image/png";
    output.bytes = encode_png(image);
    return output;
}

std::string base64_encode_bytes(const std::vector<std::uint8_t>& bytes) {
    return base64::encode(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::uint8_t> base64_decode_bytes(const std::string& encoded) {
    const std::string decoded = base64::decode(encoded);
    return std::vector<std::uint8_t>(decoded.begin(), decoded.end());
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("file_io_error: failed to open input file: " + path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_binary_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path output_path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("file_io_error: failed to open output file: " + path);
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace mcdk::image
