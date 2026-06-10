#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>

namespace th2 {

SDL_Surface* load_tga(std::span<const std::uint8_t> bytes);

}  // namespace th2

