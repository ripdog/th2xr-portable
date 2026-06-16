#include "font.hpp"

#include <SDL3_ttf/SDL_ttf.h>
#ifndef __ANDROID__
#include <fontconfig/fontconfig.h>
#endif
#include <iconv.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

namespace th2 {
namespace {

constexpr std::size_t full_glyph_count = 7023;
constexpr std::size_t full_glyph_bytes = 24 * 24 / 2;
constexpr std::size_t half_glyph_bytes = 24 * 12 / 2;
constexpr std::size_t ascii_offset = full_glyph_count * full_glyph_bytes;
constexpr std::size_t shadow_full_bytes = 14 * 28;
constexpr std::size_t shadow_half_bytes = 8 * 28;
constexpr std::size_t shadow_ascii_offset =
    4 + full_glyph_count * shadow_full_bytes;

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

std::string utf8_to_cp932(std::string_view text)
{
    iconv_t converter = iconv_open("CP932", "UTF-8");
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        converter = iconv_open("SHIFT_JIS", "UTF-8");
    }
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        throw std::runtime_error("CP932/SHIFT_JIS converter is unavailable");
    }
    std::string output(text.size() * 2 + 2, '\0');
    char* input = const_cast<char*>(text.data());
    std::size_t input_left = text.size();
    char* destination = output.data();
    std::size_t output_left = output.size();
    const auto result = iconv(
        converter, &input, &input_left, &destination, &output_left);
    iconv_close(converter);
    if (result == static_cast<std::size_t>(-1)) {
        throw std::runtime_error("invalid UTF-8 text for CP932 conversion");
    }
    output.resize(output.size() - output_left);
    return output;
}

struct FontPoint {
    int pointer;
    std::uint16_t start;
    std::uint16_t end;
};

// Mapping from Shift_JIS full-width code words to glyph indices in
// font24.fd0.  Derived from the original ToHeart2 Font.cpp.
constexpr std::array<FontPoint, 53> full_font_points{{
    {     0, 0x8141, 0x81ac },
    {   108, 0x81b8, 0x81bf },
    {   116, 0x81c8, 0x81ce },
    {   123, 0x81da, 0x81fc },
    {   158, 0x824f, 0x8258 },
    {   168, 0x8260, 0x8279 },
    {   194, 0x8281, 0x829a },
    {   220, 0x829f, 0x82f1 },
    {   303, 0x8340, 0x8396 },
    {   390, 0x839f, 0x83b6 },
    {   414, 0x83bf, 0x83d6 },
    {   438, 0x8440, 0x8460 },
    {   471, 0x8470, 0x8491 },
    {   505, 0x849f, 0x84be },
    {   537, 0x8740, 0x8799 },
    {   627, 0x889f, 0x88fc },
    {   721, 0x8940, 0x89fc },
    {   910, 0x8a40, 0x8afc },
    {  1099, 0x8b40, 0x8bfc },
    {  1288, 0x8c40, 0x8cfc },
    {  1477, 0x8d40, 0x8dfc },
    {  1666, 0x8e40, 0x8efc },
    {  1855, 0x8f40, 0x8ffc },
    {  2044, 0x9040, 0x90fc },
    {  2233, 0x9140, 0x91fc },
    {  2422, 0x9240, 0x92fc },
    {  2611, 0x9340, 0x93fc },
    {  2800, 0x9440, 0x94fc },
    {  2989, 0x9540, 0x95fc },
    {  3178, 0x9640, 0x96fc },
    {  3367, 0x9740, 0x97fc },
    {  3556, 0x9840, 0x9872 },
    {  3607, 0x989f, 0x98fc },
    {  3701, 0x9940, 0x99fc },
    {  3890, 0x9a40, 0x9afc },
    {  4079, 0x9b40, 0x9bfc },
    {  4268, 0x9c40, 0x9cfc },
    {  4457, 0x9d40, 0x9dfc },
    {  4646, 0x9e40, 0x9efc },
    {  4835, 0x9f40, 0x9ffc },
    {  5024, 0xe040, 0xe0fc },
    {  5213, 0xe140, 0xe1fc },
    {  5402, 0xe240, 0xe2fc },
    {  5591, 0xe340, 0xe3fc },
    {  5780, 0xe440, 0xe4fc },
    {  5969, 0xe540, 0xe5fc },
    {  6158, 0xe640, 0xe6fc },
    {  6347, 0xe740, 0xe7fc },
    {  6536, 0xe840, 0xe8fc },
    {  6725, 0xe940, 0xe9fc },
    {  6914, 0xea40, 0xeaa4 },
    {  7015, 0xf040, 0xf047 },
    {  7023, 0x8140, 0x8140 },
}};

bool is_cp932_full_lead(unsigned char byte)
{
    return (byte >= 0x81 && byte <= 0x9f)
        || (byte >= 0xe0 && byte <= 0xea)
        || byte == 0xf0;
}

bool is_cp932_half(unsigned char byte)
{
    return byte == ' '
        || (byte >= 0x21 && byte <= 0x7e)
        || (byte >= 0xa1 && byte <= 0xdf);
}

int cp932_half_index(unsigned char byte)
{
    if (byte >= '!' && byte <= '~') {
        return byte - '!';
    }
    if (byte >= 0xa1 && byte <= 0xdf) {
        return 94 + (byte - 0xa1);
    }
    if (byte == ' ') {
        return 157;
    }
    return -1;
}

int cp932_full_index(std::uint16_t code)
{
    // The final entry is the original engine's full-width-space sentinel.
    // It advances like a full-width character but has no bitmap.
    for (std::size_t i = 0; i + 1 < full_font_points.size(); ++i) {
        const auto& point = full_font_points[i];
        if (code >= point.start && code <= point.end) {
            return point.pointer + (code - point.start);
        }
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
    int logical_size = 0;
    float scale = 0.0f;
    std::unordered_map<
        std::string, std::unique_ptr<SDL_Texture, TextureDeleter>> textures;
#ifdef __ANDROID__
    std::unique_ptr<void, decltype(&SDL_free)> font_data_{nullptr, &SDL_free};
#endif

    ~Modern()
    {
        if (font) {
            TTF_CloseFont(font);
        }
    }

    void open(
        std::string_view requested_family, int requested_size,
        float requested_scale)
    {
        static const bool initialized = [] {
            if (!TTF_Init()) {
                throw std::runtime_error(SDL_GetError());
            }
#ifndef __ANDROID__
            if (!FcInit()) {
                throw std::runtime_error("fontconfig initialization failed");
            }
#endif
            return true;
        }();
        (void)initialized;
        if (font && family == requested_family
            && logical_size == requested_size
            && std::abs(scale - requested_scale) < 0.01f) {
            return;
        }
        if (font) {
            TTF_CloseFont(font);
            font = nullptr;
        }
#ifdef __ANDROID__
        font_data_.reset();
#endif
        textures.clear();

        std::string path(requested_family);
        if (!std::filesystem::is_regular_file(path)) {
#ifdef __ANDROID__
            path = TH2_ANDROID_FONT_PATH;
#else
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
#endif
        }

#ifdef __ANDROID__
        std::size_t font_data_size = 0;
        void* font_data = SDL_LoadFile(path.c_str(), &font_data_size);
        if (!font_data) {
            throw std::runtime_error(
                std::string("SDL_LoadFile failed for ") + path + ": "
                + SDL_GetError());
        }
        SDL_IOStream* font_io = SDL_IOFromConstMem(font_data, font_data_size);
        if (!font_io) {
            SDL_free(font_data);
            throw std::runtime_error(
                std::string("SDL_IOFromConstMem failed for ") + path + ": "
                + SDL_GetError());
        }
        // TTF_OpenFontIO takes ownership of the IO stream and (with closeio=true)
        // will free the const memory reference; we still need to free font_data
        // because SDL_IOFromConstMem does not copy it. Keep the data alive by
        // storing it in the Modern object.
        font_data_ = std::unique_ptr<void, decltype(&SDL_free)>(
            font_data, &SDL_free);
        font = TTF_OpenFontIO(font_io, true, requested_size * requested_scale);
        if (!font) {
            font_data_.reset();
            throw std::runtime_error(
                std::string("TTF_OpenFontIO failed for ") + path + ": "
                + SDL_GetError());
        }
#else
        font = TTF_OpenFont(path.c_str(), requested_size * requested_scale);
        if (!font) {
            throw std::runtime_error(
                std::string("TTF_OpenFont failed for ") + path + ": "
                + SDL_GetError());
        }
#endif
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT_SUBPIXEL);
        family = requested_family;
        logical_size = requested_size;
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
    const auto* shadow_entry = archive.find("font24.fk0");
    if (!shadow_entry) {
        throw std::runtime_error("font24.fk0 not found");
    }
    shadow_data_ = archive.read(*shadow_entry);
    if (shadow_data_.size()
        < shadow_ascii_offset + 157 * shadow_half_bytes) {
        throw std::runtime_error("font24.fk0 is truncated");
    }
    shadow_width_ = static_cast<int>(
        shadow_data_[0]
        | shadow_data_[1] << 8
        | shadow_data_[2] << 16
        | shadow_data_[3] << 24);
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
    bool authentic, std::string_view family, int font_size,
    float framebuffer_scale)
{
    authentic_ = authentic;
    family_ = family.empty() ? "sans-serif" : std::string(family);
    font_size_ = std::clamp(font_size, 12, 48);
    framebuffer_scale_ = std::max(framebuffer_scale, 1.0f);
}

float GameFont::text_width(std::string_view text) const
{
    if (authentic_ || text.empty()) {
        try {
            const auto cp932 = utf8_to_cp932(text);
            float result = 0.0f;
            for (std::size_t i = 0; i < cp932.size();) {
                const auto byte = static_cast<unsigned char>(cp932[i]);
                if (byte == '\n') {
                    break;
                }
                if (is_cp932_full_lead(byte)) {
                    if (i + 1 >= cp932.size()) {
                        break;
                    }
                    const auto code = static_cast<std::uint16_t>(
                        (byte << 8)
                        | static_cast<unsigned char>(cp932[i + 1]));
                    if (code == 0x8140 || cp932_full_index(code) >= 0) {
                        result += static_cast<float>(size);
                    }
                    i += 2;
                } else if (is_cp932_half(byte)) {
                    if (cp932_half_index(byte) >= 0) {
                        result += static_cast<float>(width);
                    }
                    ++i;
                } else {
                    ++i;
                }
            }
            return result;
        } catch (const std::exception&) {
            float result = 0.0f;
            for (const auto byte : text) {
                if (byte == '\n') {
                    break;
                }
                if (glyph(static_cast<unsigned char>(byte))) {
                    result += GameFont::width;
                }
            }
            return result;
        }
    }

    try {
        modern_->open(family_, font_size_, framebuffer_scale_);
        int pixel_width = 0;
        int pixel_height = 0;
        if (!TTF_GetStringSize(
                modern_->font, text.data(), text.size(),
                &pixel_width, &pixel_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        return static_cast<float>(pixel_width) / framebuffer_scale_;
    } catch (const std::exception& error) {
        SDL_Log("Modern font text_width fallback: %s", error.what());
        float result = 0.0f;
        for (const auto byte : text) {
            if (byte == '\n') {
                break;
            }
            if (glyph(static_cast<unsigned char>(byte))) {
                result += GameFont::width;
            }
        }
        return result;
    }
}

const std::vector<std::string>& GameFont::system_families()
{
    static const std::vector<std::string> families = [] {
        std::vector<std::string> result;
#ifdef __ANDROID__
        result.emplace_back("Liberation Serif");
#else
        if (!FcInit()) {
            return result;
        }
        FcPattern* pattern = FcPatternCreate();
        FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, nullptr);
        FcFontSet* fonts = FcFontList(nullptr, pattern, objects);
        if (fonts) {
            for (int i = 0; i < fonts->nfont; ++i) {
                FcChar8* family = nullptr;
                if (FcPatternGetString(
                        fonts->fonts[i], FC_FAMILY, 0, &family)
                    == FcResultMatch) {
                    result.emplace_back(
                        reinterpret_cast<const char*>(family));
                }
            }
            FcFontSetDestroy(fonts);
        }
        FcObjectSetDestroy(objects);
        FcPatternDestroy(pattern);
        std::ranges::sort(result);
        result.erase(std::unique(result.begin(), result.end()), result.end());
#endif
        return result;
    }();
    return families;
}

namespace {

void draw_glyph(
    SDL_Renderer* renderer, float x, float y, int glyph_width,
    const std::uint8_t* bitmap, std::uint8_t red, std::uint8_t green,
    std::uint8_t blue, std::uint8_t alpha)
{
    for (int row = 0; row < 24; ++row) {
        for (int column = 0; column < glyph_width; ++column) {
            const auto packed = bitmap[row * (glyph_width / 2) + column / 2];
            const auto coverage =
                column % 2 == 0 ? packed & 0x0f : packed >> 4;
            if (!coverage) {
                continue;
            }
            SDL_SetRenderDrawColor(
                renderer, red, green, blue,
                static_cast<std::uint8_t>(coverage * 17 * alpha / 255));
            SDL_RenderPoint(renderer, x + column, y + row);
        }
    }
}

void draw_shadow_mask(
    SDL_Renderer* renderer, float x, float y, int width, int height,
    const std::uint8_t* bitmap, std::uint8_t alpha)
{
    const int stride = (width + 1) / 2;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const auto packed = bitmap[row * stride + column / 2];
            const auto coverage =
                column % 2 == 0 ? packed & 0x0f : packed >> 4;
            if (!coverage) {
                continue;
            }
            SDL_SetRenderDrawColor(
                renderer, 0, 0, 0,
                static_cast<std::uint8_t>(coverage * 17 * alpha / 255));
            SDL_RenderPoint(renderer, x + column, y + row);
        }
    }
}

}  // namespace

void GameFont::draw_bitmap(
    SDL_Renderer* renderer, float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue,
    std::uint8_t alpha) const
{
    const float start_x = x;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    std::string cp932;
    try {
        cp932 = utf8_to_cp932(text);
    } catch (const std::exception&) {
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
            draw_glyph(
                renderer, x, y, width, bitmap, red, green, blue, alpha);
            x += width;
        }
        return;
    }

    for (std::size_t i = 0; i < cp932.size();) {
        const auto byte = static_cast<unsigned char>(cp932[i]);
        if (byte == '\n') {
            x = start_x;
            y += 31;
            ++i;
            continue;
        }
        if (is_cp932_full_lead(byte)) {
            if (i + 1 >= cp932.size()) {
                break;
            }
            const auto code = static_cast<std::uint16_t>(
                (byte << 8)
                | static_cast<unsigned char>(cp932[i + 1]));
            const auto index = cp932_full_index(code);
            if (index >= 0) {
                const auto* bitmap =
                    data_.data() + index * full_glyph_bytes;
                draw_glyph(
                    renderer, x, y, size, bitmap, red, green, blue, alpha);
                x += size;
            } else if (code == 0x8140) {
                x += size;
            }
            i += 2;
        } else if (is_cp932_half(byte)) {
            const auto index = cp932_half_index(byte);
            if (index >= 0) {
                const auto* bitmap =
                    data_.data() + ascii_offset + index * half_glyph_bytes;
                draw_glyph(
                    renderer, x, y, width, bitmap, red, green, blue, alpha);
                x += width;
            }
            ++i;
        } else {
            ++i;
        }
    }
}

void GameFont::draw(
    SDL_Renderer* renderer, float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue,
    std::uint8_t alpha) const
{
    if (authentic_ || text.empty()) {
        draw_bitmap(renderer, x, y, text, red, green, blue, alpha);
        return;
    }
    if (text.find('\n') != std::string_view::npos) {
        while (true) {
            const auto newline = text.find('\n');
            draw(
                renderer, x, y, text.substr(0, newline),
                red, green, blue, alpha);
            if (newline == std::string_view::npos) {
                break;
            }
            text.remove_prefix(newline + 1);
            y += 31.0f;
        }
        return;
    }

    try {
        modern_->open(family_, font_size_, framebuffer_scale_);
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
        SDL_SetTextureAlphaMod(found->second.get(), alpha);
        const SDL_FRect destination{
            x, y,
            texture_width / framebuffer_scale_,
            texture_height / framebuffer_scale_};
        SDL_RenderTexture(
            renderer, found->second.get(), nullptr, &destination);
        SDL_SetTextureAlphaMod(found->second.get(), 255);
    } catch (const std::exception& error) {
        SDL_Log("Modern font draw fallback: %s", error.what());
        draw_bitmap(renderer, x, y, text, red, green, blue, alpha);
    }
}

void GameFont::draw_original(
    SDL_Renderer* renderer, float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue,
    std::uint8_t alpha) const
{
    draw_bitmap(renderer, x, y, text, red, green, blue, alpha);
}

void GameFont::draw_authentic_shadow(
    SDL_Renderer* renderer, float x, float y, std::string_view text,
    std::uint8_t alpha) const
{
    if (!authentic_ || shadow_width_ <= 0 || text.empty()) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const auto cp932 = utf8_to_cp932(text);
    for (std::size_t i = 0; i < cp932.size();) {
        const auto byte = static_cast<unsigned char>(cp932[i]);
        if (is_cp932_full_lead(byte)) {
            if (i + 1 >= cp932.size()) {
                break;
            }
            const auto code = static_cast<std::uint16_t>(
                (byte << 8)
                | static_cast<unsigned char>(cp932[i + 1]));
            const auto index = cp932_full_index(code);
            if (index >= 0) {
                draw_shadow_mask(
                    renderer,
                    x - shadow_width_ + 1.0f,
                    y - shadow_width_ + 1.0f,
                    size + shadow_width_ * 2,
                    size + shadow_width_ * 2,
                    shadow_data_.data() + 4
                        + index * shadow_full_bytes,
                    alpha);
                x += size;
            } else if (code == 0x8140) {
                x += size;
            }
            i += 2;
        } else if (is_cp932_half(byte)) {
            const auto index = cp932_half_index(byte);
            if (index >= 0 && index < 157) {
                draw_shadow_mask(
                    renderer,
                    x - shadow_width_ + 1.0f,
                    y - shadow_width_ + 1.0f,
                    width + shadow_width_ * 2,
                    size + shadow_width_ * 2,
                    shadow_data_.data() + shadow_ascii_offset
                        + index * shadow_half_bytes,
                    alpha);
            }
            x += width;
            ++i;
        } else {
            ++i;
        }
    }
}

}  // namespace th2
