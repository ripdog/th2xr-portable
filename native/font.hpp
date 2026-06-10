#pragma once

#include "archive.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace th2 {

class GameFont {
public:
    explicit GameFont(const Archive& archive);

    int glyph_width(unsigned char character) const;
    void draw(
        SDL_Renderer* renderer, float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255) const;

private:
    static constexpr int size = 24;
    static constexpr int width = 12;
    std::vector<std::uint8_t> data_;

    const std::uint8_t* glyph(unsigned char character) const;
};

}  // namespace th2
