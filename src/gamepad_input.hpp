#pragma once

#include <SDL3/SDL.h>
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
    bool last_event_was_gamepad() const { return last_event_was_gamepad_; }

private:
    bool right_trigger_held_ = false;
    bool last_event_was_gamepad_ = false;
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> last_unhandled_gamepad_axes_{};
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> last_unhandled_joystick_axes_{};
    std::vector<GamepadPtr> gamepads_;
};

}  // namespace th2app
