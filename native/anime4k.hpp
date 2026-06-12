#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

namespace th2 {

class Anime4K {
public:
    Anime4K(SDL_Renderer* renderer, const std::filesystem::path& shader_dir);
    ~Anime4K();

    Anime4K(const Anime4K&) = delete;
    Anime4K& operator=(const Anime4K&) = delete;

    bool available() const;
    SDL_Texture* art_target() const;
    SDL_Texture* overlay_target();
    void present();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
