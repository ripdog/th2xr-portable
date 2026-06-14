#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace th2 {

SDL_Surface* load_image(
    std::span<const std::uint8_t> bytes, std::string_view name);
SDL_Surface* load_tga(std::span<const std::uint8_t> bytes);
void apply_tone_curve(
    SDL_Surface* surface,
    std::span<const std::uint8_t> curve,
    int vividness);

}  // namespace th2
