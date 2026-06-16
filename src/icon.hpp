#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

namespace th2 {

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};

using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

SurfacePtr load_executable_icon(const std::filesystem::path& executable);

}  // namespace th2
