#pragma once

#include "archive.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace th2 {

class GameFont {
public:
    explicit GameFont(const Archive& archive);
    ~GameFont();

    int glyph_width(unsigned char character) const;
    void configure(bool authentic, std::string_view family,
                   float framebuffer_scale);
    void draw(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255) const;

private:
    struct Modern;
    static constexpr int size = 24;
    static constexpr int width = 12;
    std::vector<std::uint8_t> data_;
    std::unique_ptr<Modern> modern_;
    bool authentic_ = false;
    std::string family_;
    float framebuffer_scale_ = 1.0f;

    const std::uint8_t* glyph(unsigned char character) const;
    void draw_bitmap(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red, std::uint8_t green, std::uint8_t blue) const;
};

}  // namespace th2
