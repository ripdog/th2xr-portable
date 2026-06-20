#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <memory>
#include <vector>

namespace th2app {

struct GamepadDeleter {
    void operator()(SDL_Gamepad* gamepad) const;
};

using GamepadPtr = std::unique_ptr<SDL_Gamepad, GamepadDeleter>;

class GamepadInput {
public:
    bool process_event(SDL_Event& event);
    bool ctrl_skip_held() const { return right_trigger_held_; }

private:
    bool right_trigger_held_ = false;
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> last_unhandled_gamepad_axes_{};
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> last_unhandled_joystick_axes_{};
    std::vector<GamepadPtr> gamepads_;
};

}  // namespace th2app
