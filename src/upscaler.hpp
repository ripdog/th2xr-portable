#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

namespace th2 {

// Abstraction that owns the fixed 800x600 core art render target and a
// monitor-resolution overlay target, and composites both to the window.
// Different implementations can upscale the art layer in different ways
// (e.g. bilinear, Anime4K, Lanczos, etc.) while the core game rendering
// logic stays locked to 800x600.
class Upscaler {
public:
    virtual ~Upscaler() = default;

    // Fixed 800x600 target for all core game art (backgrounds, characters,
    // transitions, title screen, map, etc.).
    virtual SDL_Texture* art_target() const = 0;

    // Monitor-resolution target for text, ImGui, and other overlays that
    // must remain crisp at the native display resolution.
    virtual SDL_Texture* overlay_target() = 0;

    // Fixed-resolution target for authentic bitmap text. This is composited
    // with linear filtering between the art and native overlay layers.
    virtual SDL_Texture* authentic_text_target() const = 0;

    // Composite the art and overlay layers to the window backbuffer.
    virtual void present() = 0;

    // Returns true if this upscaler is the Anime4K implementation.
    virtual bool is_anime4k() const { return false; }
};

// Creates the best available upscaler. If use_anime4k is true and the
// Anime4K GPU path is available, returns that; otherwise returns a linear
// fallback. If anime4k_available is non-null, it is set to true when the
// Anime4K path could be initialized.
std::unique_ptr<Upscaler> create_upscaler(
    SDL_Renderer* renderer,
    const std::filesystem::path& shader_dir,
    bool use_anime4k,
    bool* anime4k_available = nullptr);

}  // namespace th2
