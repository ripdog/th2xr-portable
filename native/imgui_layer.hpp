#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace th2 {

class ImGuiLayer {
public:
    ImGuiLayer(SDL_Window* window, SDL_Renderer* renderer);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void process_event(const SDL_Event& event);
    void new_frame(float framebuffer_scale_x = 1.0f,
                   float framebuffer_scale_y = 1.0f);
    void render();
    bool wants_input() const;
    bool wants_mouse() const;

private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::uint64_t last_frame_ticks_ = 0;
};

}  // namespace th2
