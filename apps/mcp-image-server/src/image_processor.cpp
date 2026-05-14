#include "image_processor.hpp"

#include "base64.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

struct RgbColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
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

std::uint32_t quantized_rgb_key(const ImageBuffer& source, int x, int y) {
    const std::size_t index = (static_cast<std::size_t>(y) * source.width + x) * 4;
    const auto r = static_cast<std::uint32_t>(source.rgba[index + 0] & 0xf0);
    const auto g = static_cast<std::uint32_t>(source.rgba[index + 1] & 0xf0);
    const auto b = static_cast<std::uint32_t>(source.rgba[index + 2] & 0xf0);
    return (r << 16) | (g << 8) | b;
}

RgbColor color_from_key(std::uint32_t key) {
    return RgbColor{
        static_cast<std::uint8_t>((key >> 16) & 0xff),
        static_cast<std::uint8_t>((key >> 8) & 0xff),
        static_cast<std::uint8_t>(key & 0xff)
    };
}

int color_distance(const ImageBuffer& source, std::size_t index, RgbColor color) {
    return std::abs(static_cast<int>(source.rgba[index + 0]) - static_cast<int>(color.r))
        + std::abs(static_cast<int>(source.rgba[index + 1]) - static_cast<int>(color.g))
        + std::abs(static_cast<int>(source.rgba[index + 2]) - static_cast<int>(color.b));
}

std::vector<RgbColor> detect_border_background_colors(const ImageBuffer& source) {
    std::map<std::uint32_t, int> histogram;

    for (int x = 0; x < source.width; ++x) {
        ++histogram[quantized_rgb_key(source, x, 0)];
        ++histogram[quantized_rgb_key(source, x, source.height - 1)];
    }
    for (int y = 0; y < source.height; ++y) {
        ++histogram[quantized_rgb_key(source, 0, y)];
        ++histogram[quantized_rgb_key(source, source.width - 1, y)];
    }

    std::vector<std::pair<int, std::uint32_t>> ranked;
    ranked.reserve(histogram.size());
    for (const auto& [key, count] : histogram) {
        ranked.emplace_back(count, key);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first > rhs.first;
    });

    std::vector<RgbColor> colors;
    for (const auto& [count, key] : ranked) {
        (void)count;
        colors.push_back(color_from_key(key));
        if (colors.size() >= 2) {
            break;
        }
    }
    return colors;
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

    if (processing.contains("chromaKey") && processing["chromaKey"].is_object()) {
        const Json& chroma = processing["chromaKey"];
        options.chroma_key_enabled = json_bool_or(chroma, "enabled", false);
        options.chroma_key_r = json_int_or(chroma, "r", 0);
        options.chroma_key_g = json_int_or(chroma, "g", 255);
        options.chroma_key_b = json_int_or(chroma, "b", 0);
        options.chroma_key_tolerance = json_int_or(chroma, "tolerance", 72);
        options.chroma_key_softness = json_int_or(chroma, "softness", 32);
        options.chroma_key_spill_suppression = json_int_or(chroma, "spillSuppression", 48);
    }

    if (processing.contains("removeFakeTransparency") && processing["removeFakeTransparency"].is_object()) {
        const Json& fake = processing["removeFakeTransparency"];
        options.remove_fake_transparency_enabled = json_bool_or(fake, "enabled", false);
        options.fake_transparency_tolerance = json_int_or(fake, "tolerance", 16);
        options.fake_transparency_alpha = json_int_or(fake, "alpha", 0);
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

ImageBuffer paste_center_rgba(const ImageBuffer& foreground, int target_width, int target_height) {
    ImageBuffer output;
    output.width = target_width;
    output.height = target_height;
    output.rgba.assign(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height) * 4, 0);

    const int offset_x = std::max(0, (target_width - foreground.width) / 2);
    const int offset_y = std::max(0, (target_height - foreground.height) / 2);
    for (int y = 0; y < foreground.height && y + offset_y < target_height; ++y) {
        for (int x = 0; x < foreground.width && x + offset_x < target_width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * foreground.width + x) * 4;
            const std::size_t dst = (static_cast<std::size_t>(y + offset_y) * target_width + (x + offset_x)) * 4;
            std::copy_n(foreground.rgba.begin() + static_cast<std::ptrdiff_t>(src), 4, output.rgba.begin() + static_cast<std::ptrdiff_t>(dst));
        }
    }
    return output;
}

ImageBuffer fit_resize_preserve_aspect(const ImageBuffer& source, int target_width, int target_height) {
    const double scale_x = static_cast<double>(target_width) / static_cast<double>(source.width);
    const double scale_y = static_cast<double>(target_height) / static_cast<double>(source.height);
    const double scale = std::min(scale_x, scale_y);
    const int fit_width = std::max(1, static_cast<int>(source.width * scale));
    const int fit_height = std::max(1, static_cast<int>(source.height * scale));
    ImageBuffer resized = nearest_resize(source, fit_width, fit_height);
    return paste_center_rgba(resized, target_width, target_height);
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
    return fit_resize_preserve_aspect(cropped, target_width, target_height);
}

ImageBuffer remove_fake_transparency_background(const ImageBuffer& source, int tolerance, int target_alpha) {
    ImageBuffer output = source;
    const std::vector<RgbColor> background_colors = detect_border_background_colors(source);
    if (background_colors.empty()) {
        return output;
    }

    const int threshold = std::max(0, tolerance) * 3;
    const std::uint8_t alpha = static_cast<std::uint8_t>(std::clamp(target_alpha, 0, 255));

    for (int y = 0; y < output.height; ++y) {
        for (int x = 0; x < output.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * output.width + x) * 4;
            const bool near_background = std::any_of(background_colors.begin(), background_colors.end(), [&](RgbColor color) {
                return color_distance(output, index, color) <= threshold;
            });
            if (near_background) {
                output.rgba[index + 0] = 0;
                output.rgba[index + 1] = 0;
                output.rgba[index + 2] = 0;
                output.rgba[index + 3] = alpha;
            }
        }
    }

    return output;
}

ImageBuffer remove_chroma_key_background(const ImageBuffer& source, int key_r, int key_g, int key_b, int tolerance, int softness, int spill_suppression) {
    ImageBuffer output = source;
    const int kr = std::clamp(key_r, 0, 255);
    const int kg = std::clamp(key_g, 0, 255);
    const int kb = std::clamp(key_b, 0, 255);
    const int hard = std::max(0, tolerance);
    const int soft = std::max(1, softness);
    const int spill = std::clamp(spill_suppression, 0, 255);

    for (int y = 0; y < output.height; ++y) {
        for (int x = 0; x < output.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * output.width + x) * 4;
            const int r = output.rgba[index + 0];
            const int g = output.rgba[index + 1];
            const int b = output.rgba[index + 2];
            const int dr = r - kr;
            const int dg = g - kg;
            const int db = b - kb;
            const double distance = std::sqrt(static_cast<double>(dr * dr + dg * dg + db * db));

            double alpha = 1.0;
            if (distance <= static_cast<double>(hard)) {
                alpha = 0.0;
            } else if (distance <= static_cast<double>(hard + soft)) {
                alpha = (distance - static_cast<double>(hard)) / static_cast<double>(soft);
            }

            // Pure chroma pixels become transparent. Mixed semi-transparent
            // pixels keep a fractional alpha and are un-premixed from the green
            // screen: observed = fg * alpha + key * (1-alpha).
            if (alpha <= 0.0) {
                output.rgba[index + 0] = 0;
                output.rgba[index + 1] = 0;
                output.rgba[index + 2] = 0;
                output.rgba[index + 3] = 0;
                continue;
            }

            if (alpha < 1.0) {
                const auto unmix = [&](int observed, int key) {
                    const double value = (static_cast<double>(observed) - static_cast<double>(key) * (1.0 - alpha)) / alpha;
                    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(value)), 0, 255));
                };
                output.rgba[index + 0] = unmix(r, kr);
                output.rgba[index + 1] = unmix(g, kg);
                output.rgba[index + 2] = unmix(b, kb);
                output.rgba[index + 3] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(alpha * 255.0)), 0, 255));
            }

            const int rr = output.rgba[index + 0];
            const int gg = output.rgba[index + 1];
            const int bb = output.rgba[index + 2];
            if (spill > 0 && gg > rr && gg > bb) {
                const int max_non_green = std::max(rr, bb);
                const int excess_green = std::max(0, gg - max_non_green);
                output.rgba[index + 1] = static_cast<std::uint8_t>(std::max(max_non_green, gg - std::min(excess_green, spill)));
            }
        }
    }

    return output;
}

ImageData process_image_data(const ImageData& input, const ImageProcessingOptions& options) {
    if (!options.enabled && !options.force_png_output) {
        return input;
    }

    ImageBuffer image = decode_image_rgba(input.bytes);

    if (options.chroma_key_enabled) {
        image = remove_chroma_key_background(
            image,
            options.chroma_key_r,
            options.chroma_key_g,
            options.chroma_key_b,
            options.chroma_key_tolerance,
            options.chroma_key_softness,
            options.chroma_key_spill_suppression
        );
    }

    if (options.remove_fake_transparency_enabled) {
        image = remove_fake_transparency_background(
            image,
            options.fake_transparency_tolerance,
            options.fake_transparency_alpha
        );
    }

    bool already_resized_to_nearest_target = false;
    if (options.transparent_domain_scale_enabled) {
        image = transparent_domain_scale(
            image,
            options.transparent_target_width,
            options.transparent_target_height,
            options.transparent_padding,
            options.alpha_threshold
        );
    } else if (options.chroma_key_enabled && options.nearest_resize_enabled && options.nearest_target_width > 0 && options.nearest_target_height > 0) {
        // Chroma-key workflows create real alpha before resizing. If the caller
        // only requested a final nearest resize, automatically crop to the
        // non-transparent domain first so the subject does not stay tiny in the
        // full green-screen canvas.
        const int auto_padding = std::max(2, std::min(image.width, image.height) / 64);
        image = transparent_domain_scale(
            image,
            options.nearest_target_width,
            options.nearest_target_height,
            auto_padding,
            std::max(1, options.alpha_threshold)
        );
        already_resized_to_nearest_target = true;
    }

    if (options.nearest_resize_enabled && !already_resized_to_nearest_target) {
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
    const std::filesystem::path input_path = std::filesystem::u8path(path);
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("file_io_error: failed to open input file: " + path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_binary_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path output_path = std::filesystem::u8path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("file_io_error: failed to open output file: " + path);
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace mcdk::image
