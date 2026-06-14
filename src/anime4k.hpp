#pragma once

#include "upscaler.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

namespace th2 {

class Anime4K : public Upscaler {
public:
    Anime4K(SDL_Renderer* renderer, const std::filesystem::path& shader_dir);
    ~Anime4K() override;

    Anime4K(const Anime4K&) = delete;
    Anime4K& operator=(const Anime4K&) = delete;
    Anime4K(Anime4K&&) noexcept;
    Anime4K& operator=(Anime4K&&) noexcept;

    bool available() const;
    SDL_Texture* art_target() const override;
    SDL_Texture* overlay_target() override;
    void present() override;
    bool is_anime4k() const override { return true; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
