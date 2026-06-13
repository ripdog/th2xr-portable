#include "upscaler.hpp"

#include "anime4k.hpp"

#include <algorithm>
#include <stdexcept>

namespace th2 {
namespace {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

SDL_FRect letterbox_rect(int output_width, int output_height)
{
    const float scale = std::min(
        output_width / 800.0f, output_height / 600.0f);
    const float width = 800.0f * scale;
    const float height = 600.0f * scale;
    return {
        (output_width - width) / 2.0f,
        (output_height - height) / 2.0f,
        width,
        height};
}

class LinearUpscaler : public Upscaler {
public:
    explicit LinearUpscaler(SDL_Renderer* renderer)
        : renderer_(renderer)
    {
        art_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            800, 600));
        if (!art_) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(art_.get(), SDL_BLENDMODE_BLEND);
    }

    SDL_Texture* art_target() const override { return art_.get(); }

    SDL_Texture* overlay_target() override
    {
        ensure_overlay();
        return overlay_.get();
    }

    void present() override
    {
        ensure_overlay();
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(
                renderer_, &output_width, &output_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        const auto destination = letterbox_rect(output_width, output_height);

        SDL_SetRenderTarget(renderer_, nullptr);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_SetTextureScaleMode(art_.get(), SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(renderer_, art_.get(), nullptr, &destination);
        SDL_RenderTexture(renderer_, overlay_.get(), nullptr, &destination);
    }

private:
    void ensure_overlay()
    {
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(
                renderer_, &output_width, &output_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        const auto destination = letterbox_rect(output_width, output_height);
        const int width = static_cast<int>(destination.w);
        const int height = static_cast<int>(destination.h);
        if (overlay_ && width == overlay_width_ && height == overlay_height_) {
            return;
        }
        overlay_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            width, height));
        if (!overlay_) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(overlay_.get(), SDL_BLENDMODE_BLEND);
        overlay_width_ = width;
        overlay_height_ = height;
    }

    SDL_Renderer* renderer_;
    Texture art_;
    Texture overlay_;
    int overlay_width_ = 0;
    int overlay_height_ = 0;
};

}  // namespace

std::unique_ptr<Upscaler> create_upscaler(
    SDL_Renderer* renderer,
    const std::filesystem::path& shader_dir,
    bool use_anime4k,
    bool* anime4k_available)
{
    auto anime4k = std::make_unique<Anime4K>(renderer, shader_dir);
    const bool available = anime4k->available();
    if (anime4k_available) {
        *anime4k_available = available;
    }
    if (use_anime4k && available) {
        return anime4k;
    }
    return std::make_unique<LinearUpscaler>(renderer);
}

}  // namespace th2
