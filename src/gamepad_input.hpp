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

private:
    std::vector<GamepadPtr> gamepads_;
};

}  // namespace th2app
