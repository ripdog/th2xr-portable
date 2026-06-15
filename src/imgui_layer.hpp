#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

namespace th2 {

class ImGuiLayer {
public:
    ImGuiLayer(SDL_Window* window, SDL_Renderer* renderer);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void process_event(const SDL_Event& event);
    void new_frame(SDL_Window* window, float display_scale = 1.0f);
    void render();
    bool wants_input() const;
    bool wants_mouse() const;
    void rebuild_font_atlas(float display_scale);

private:

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::uint64_t last_frame_ticks_ = 0;
    std::string imgui_font_path_;
    float display_scale_ = 1.0f;
    float last_font_scale_ = 0.0f;
#ifdef __ANDROID__
    // ImGui's AddFontFromMemoryTTF needs the TTF data to stay alive until
    // the atlas is built; we free it when the atlas is rebuilt or destroyed.
    std::unique_ptr<void, decltype(&SDL_free)> imgui_font_data_{nullptr, SDL_free};
#endif
};

}  // namespace th2
