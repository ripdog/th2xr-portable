#include "image.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace th2 {
namespace {

std::uint16_t read_u16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

}  // namespace

SDL_Surface* load_image(
    std::span<const std::uint8_t> bytes, std::string_view name)
{
    std::string extension;
    if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) {
        extension.assign(name.substr(dot));
        for (char& byte : extension) {
            byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
        }
    }
    if (extension == ".tga") {
        return load_tga(bytes);
    }
    if (extension == ".bmp") {
        SDL_IOStream* input = SDL_IOFromConstMem(bytes.data(), bytes.size());
        if (!input) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Surface* loaded = SDL_LoadBMP_IO(input, true);
        if (!loaded) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Surface* surface =
            SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(loaded);
        if (!surface) {
            throw std::runtime_error(SDL_GetError());
        }
        return surface;
    }
    throw std::runtime_error("unsupported image extension: " + extension);
}

SDL_Surface* load_tga(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 18) {
        throw std::runtime_error("truncated TGA header");
    }

    const int id_length = bytes[0];
    const int color_map_type = bytes[1];
    const int image_type = bytes[2];
    const int color_map_length = read_u16(bytes.data() + 5);
    const int color_map_depth = bytes[7];
    const int width = read_u16(bytes.data() + 12);
    const int height = read_u16(bytes.data() + 14);
    const int depth = bytes[16];
    const bool top_origin = (bytes[17] & 0x20) != 0;

    if (width <= 0 || height <= 0 || (image_type != 1 && image_type != 2)) {
        throw std::runtime_error("unsupported TGA image type");
    }
    if ((image_type == 1 && (depth != 8 || color_map_type != 1))
        || (image_type == 2 && depth != 24 && depth != 32)) {
        throw std::runtime_error("unsupported TGA pixel format");
    }

    std::size_t position = 18 + id_length;
    std::vector<std::array<std::uint8_t, 4>> palette;
    if (color_map_type) {
        const std::size_t palette_pixel_size = color_map_depth / 8;
        if ((palette_pixel_size != 3 && palette_pixel_size != 4)
            || position + color_map_length * palette_pixel_size > bytes.size()) {
            throw std::runtime_error("invalid TGA palette");
        }
        palette.reserve(color_map_length);
        for (int i = 0; i < color_map_length; ++i) {
            const auto* pixel = bytes.data() + position;
            palette.push_back({pixel[2], pixel[1], pixel[0],
                               static_cast<std::uint8_t>(palette_pixel_size == 4 ? pixel[3] : 255)});
            position += palette_pixel_size;
        }
    }

    const std::size_t source_pixel_size = depth / 8;
    if (position + static_cast<std::size_t>(width) * height * source_pixel_size > bytes.size()) {
        throw std::runtime_error("truncated TGA pixels");
    }

    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        throw std::runtime_error(SDL_GetError());
    }
    auto* destination = static_cast<std::uint32_t*>(surface->pixels);
    const int destination_pitch = surface->pitch / sizeof(std::uint32_t);

    for (int source_y = 0; source_y < height; ++source_y) {
        const int destination_y = top_origin ? source_y : height - source_y - 1;
        for (int x = 0; x < width; ++x) {
            std::array<std::uint8_t, 4> color{};
            if (image_type == 1) {
                const auto index = bytes[position++];
                if (index >= palette.size()) {
                    SDL_DestroySurface(surface);
                    throw std::runtime_error("TGA palette index out of range");
                }
                color = palette[index];
            } else {
                color = {bytes[position + 2], bytes[position + 1], bytes[position],
                         static_cast<std::uint8_t>(depth == 32 ? bytes[position + 3] : 255)};
                position += source_pixel_size;
            }
            destination[destination_y * destination_pitch + x]
                = SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format), nullptr,
                              color[0], color[1], color[2], color[3]);
        }
    }
    return surface;
}

void apply_tone_curve(
    SDL_Surface* surface,
    std::span<const std::uint8_t> curve,
    int vividness)
{
    if (!surface) {
        throw std::invalid_argument("tone curve surface is null");
    }
    if (!curve.empty() && curve.size() != 256
        && curve.size() != 768 && curve.size() != 1280) {
        throw std::runtime_error("invalid tone curve size");
    }

    vividness = std::clamp(vividness, 0, 256);
    if (surface->format != SDL_PIXELFORMAT_RGBA32) {
        throw std::runtime_error("tone curve requires an RGBA32 surface");
    }
    auto map_channel = [&](std::uint8_t value, int channel) {
        if (curve.empty()) {
            return value;
        }
        if (curve.size() == 256) {
            return curve[value];
        }
        if (curve.size() == 768) {
            return curve[static_cast<std::size_t>(channel) * 256 + value];
        }
        const auto channel_value =
            curve[256 + static_cast<std::size_t>(channel) * 256 + value];
        return curve[channel_value];
    };

    const auto* format = SDL_GetPixelFormatDetails(surface->format);
    const auto* palette = SDL_GetSurfacePalette(surface);
    auto* pixels = static_cast<std::uint32_t*>(surface->pixels);
    const auto pitch = surface->pitch / sizeof(std::uint32_t);
    for (int y = 0; y < surface->h; ++y) {
        for (int x = 0; x < surface->w; ++x) {
            auto& pixel = pixels[static_cast<std::size_t>(y) * pitch + x];
            std::uint8_t red = 0;
            std::uint8_t green = 0;
            std::uint8_t blue = 0;
            std::uint8_t alpha = 0;
            SDL_GetRGBA(
                pixel, format, palette, &red, &green, &blue, &alpha);
            if (vividness != 256) {
                const int gray = (red * 77 + green * 28 + blue * 151) >> 8;
                red = static_cast<std::uint8_t>(
                    (gray * (256 - vividness) + red * vividness) >> 8);
                green = static_cast<std::uint8_t>(
                    (gray * (256 - vividness) + green * vividness) >> 8);
                blue = static_cast<std::uint8_t>(
                    (gray * (256 - vividness) + blue * vividness) >> 8);
            }
            pixel = SDL_MapRGBA(
                format, palette,
                map_channel(red, 0),
                map_channel(green, 1),
                map_channel(blue, 2),
                alpha);
        }
    }
}

}  // namespace th2
