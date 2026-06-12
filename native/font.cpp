#include "font.hpp"

#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

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

struct GameFont::Modern {
    struct TextureDeleter {
        void operator()(SDL_Texture* texture) const
        {
            SDL_DestroyTexture(texture);
        }
    };

    TTF_Font* font = nullptr;
    std::string family;
    float scale = 0.0f;
    std::unordered_map<
        std::string, std::unique_ptr<SDL_Texture, TextureDeleter>> textures;

    ~Modern()
    {
        if (font) {
            TTF_CloseFont(font);
        }
    }

    void open(std::string_view requested_family, float requested_scale)
    {
        static const bool initialized = [] {
            if (!TTF_Init()) {
                throw std::runtime_error(SDL_GetError());
            }
            if (!FcInit()) {
                throw std::runtime_error("fontconfig initialization failed");
            }
            return true;
        }();
        (void)initialized;
        if (font && family == requested_family
            && std::abs(scale - requested_scale) < 0.01f) {
            return;
        }
        if (font) {
            TTF_CloseFont(font);
            font = nullptr;
        }
        textures.clear();

        std::string path(requested_family);
        if (!std::filesystem::is_regular_file(path)) {
            FcPattern* pattern = FcNameParse(
                reinterpret_cast<const FcChar8*>(path.c_str()));
            if (!pattern) {
                throw std::runtime_error("cannot parse font family");
            }
            FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);
            FcResult result = FcResultNoMatch;
            FcPattern* match = FcFontMatch(nullptr, pattern, &result);
            FcPatternDestroy(pattern);
            FcChar8* matched_path = nullptr;
            if (!match
                || FcPatternGetString(
                       match, FC_FILE, 0, &matched_path) != FcResultMatch) {
                if (match) {
                    FcPatternDestroy(match);
                }
                throw std::runtime_error(
                    "font family not found: " + std::string(requested_family));
            }
            path = reinterpret_cast<const char*>(matched_path);
            FcPatternDestroy(match);
        }

        font = TTF_OpenFont(path.c_str(), size * requested_scale);
        if (!font) {
            throw std::runtime_error(SDL_GetError());
        }
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT_SUBPIXEL);
        family = requested_family;
        scale = requested_scale;
    }
};

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
    modern_ = std::make_unique<Modern>();
}

GameFont::~GameFont() = default;

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

void GameFont::configure(
    bool authentic, std::string_view family, float framebuffer_scale)
{
    authentic_ = authentic;
    family_ = family.empty() ? "sans-serif" : std::string(family);
    framebuffer_scale_ = std::max(framebuffer_scale, 1.0f);
}

void GameFont::draw_bitmap(
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

void GameFont::draw(
    SDL_Renderer* renderer, float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue) const
{
    if (authentic_ || text.empty()) {
        draw_bitmap(renderer, x, y, text, red, green, blue);
        return;
    }

    try {
        modern_->open(family_, framebuffer_scale_);
        const SDL_Color color{red, green, blue, 255};
        std::string key(text);
        key.append({
            static_cast<char>(red),
            static_cast<char>(green),
            static_cast<char>(blue),
        });
        auto found = modern_->textures.find(key);
        if (found == modern_->textures.end()) {
            auto* surface = TTF_RenderText_Blended(
                modern_->font, text.data(), text.size(), color);
            if (!surface) {
                throw std::runtime_error(SDL_GetError());
            }
            auto* texture = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_DestroySurface(surface);
            if (!texture) {
                throw std::runtime_error(SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            if (modern_->textures.size() >= 512) {
                modern_->textures.clear();
            }
            found = modern_->textures.emplace(key, texture).first;
        }
        float texture_width = 0.0f;
        float texture_height = 0.0f;
        SDL_GetTextureSize(
            found->second.get(), &texture_width, &texture_height);
        const SDL_FRect destination{
            x, y,
            texture_width / framebuffer_scale_,
            texture_height / framebuffer_scale_};
        SDL_RenderTexture(
            renderer, found->second.get(), nullptr, &destination);
    } catch (const std::exception&) {
        draw_bitmap(renderer, x, y, text, red, green, blue);
    }
}

}  // namespace th2
