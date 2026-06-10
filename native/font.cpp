#include "font.hpp"

#include <array>
#include <stdexcept>

namespace th2 {
namespace {

constexpr std::size_t full_glyph_count = 7023;
constexpr std::size_t full_glyph_bytes = 24 * 24 / 2;
constexpr std::size_t half_glyph_bytes = 24 * 12 / 2;
constexpr std::size_t ascii_offset = full_glyph_count * full_glyph_bytes;

int glyph_index(unsigned char character)
{
    if (character >= '!' && character <= '~') {
        return character - '!';
    }
    if (character == ' ') {
        return 157;
    }
    return -1;
}

}  // namespace

GameFont::GameFont(const Archive& archive)
{
    const auto* entry = archive.find("font24.fd0");
    if (!entry) {
        throw std::runtime_error("font24.fd0 not found");
    }
    data_ = archive.read(*entry);
    if (data_.size() < ascii_offset + 158 * half_glyph_bytes) {
        throw std::runtime_error("font24.fd0 is truncated");
    }
}

const std::uint8_t* GameFont::glyph(unsigned char character) const
{
    const auto index = glyph_index(character);
    return index < 0 ? nullptr
        : data_.data() + ascii_offset + index * half_glyph_bytes;
}

int GameFont::glyph_width(unsigned char character) const
{
    return glyph(character) ? width : 0;
}

void GameFont::draw(
    SDL_Renderer* renderer, float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue) const
{
    const float start_x = x;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const auto byte : text) {
        if (byte == '\n') {
            x = start_x;
            y += 31;
            continue;
        }
        const auto* bitmap = glyph(static_cast<unsigned char>(byte));
        if (!bitmap) {
            continue;
        }
        for (int row = 0; row < size; ++row) {
            for (int column = 0; column < width; ++column) {
                const auto packed = bitmap[row * (width / 2) + column / 2];
                const auto coverage = column % 2 == 0 ? packed & 0x0f : packed >> 4;
                if (!coverage) {
                    continue;
                }
                SDL_SetRenderDrawColor(
                    renderer, red, green, blue,
                    static_cast<std::uint8_t>(coverage * 17));
                SDL_RenderPoint(renderer, x + column, y + row);
            }
        }
        x += width;
    }
}

}  // namespace th2
