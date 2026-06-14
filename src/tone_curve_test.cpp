#include "image.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

int main()
{
    SDL_Surface* surface =
        SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return 1;
    }
    SDL_WriteSurfacePixel(surface, 0, 0, 100, 50, 20, 200);

    std::array<std::uint8_t, 768> curve{};
    for (int value = 0; value < 256; ++value) {
        curve[value] = static_cast<std::uint8_t>(255 - value);
        curve[256 + value] = static_cast<std::uint8_t>(value / 2);
        curve[512 + value] = static_cast<std::uint8_t>(value);
    }
    th2::apply_tone_curve(surface, curve, 256);
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;
    SDL_ReadSurfacePixel(
        surface, 0, 0, &red, &green, &blue, &alpha);
    if (red != 155 || green != 25 || blue != 20 || alpha != 200) {
        SDL_DestroySurface(surface);
        return 2;
    }

    SDL_WriteSurfacePixel(surface, 0, 0, 100, 50, 20, 200);
    th2::apply_tone_curve(surface, {}, 0);
    SDL_ReadSurfacePixel(
        surface, 0, 0, &red, &green, &blue, &alpha);
    const auto gray = static_cast<std::uint8_t>(
        (100 * 77 + 50 * 28 + 20 * 151) >> 8);
    const bool correct =
        red == gray && green == gray && blue == gray && alpha == 200;
    SDL_DestroySurface(surface);
    return correct ? 0 : 3;
}
