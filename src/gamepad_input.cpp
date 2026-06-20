#include "gamepad_input.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <optional>

namespace th2app {

namespace {

constexpr Sint16 TriggerThreshold = 16000;

SDL_Scancode scancode_for_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RETURN: return SDL_SCANCODE_RETURN;
    case SDLK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case SDLK_PAGEUP: return SDL_SCANCODE_PAGEUP;
    case SDLK_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
    case SDLK_UP: return SDL_SCANCODE_UP;
    case SDLK_DOWN: return SDL_SCANCODE_DOWN;
    case SDLK_LEFT: return SDL_SCANCODE_LEFT;
    case SDLK_RIGHT: return SDL_SCANCODE_RIGHT;
    case SDLK_F8: return SDL_SCANCODE_F8;
    case SDLK_F9: return SDL_SCANCODE_F9;
    case SDLK_F10: return SDL_SCANCODE_F10;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

std::optional<SDL_Event> gamepad_button_as_key_event(
    const SDL_GamepadButtonEvent& button)
{
    SDL_Keycode key = SDLK_UNKNOWN;
    switch (button.button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        key = SDLK_RETURN;
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_START:
        key = SDLK_ESCAPE;
        break;
    case SDL_GAMEPAD_BUTTON_BACK:
        key = SDLK_F8;
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        key = SDLK_F9;
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        key = SDLK_PAGEUP;
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        key = SDLK_F10;
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        key = SDLK_PAGEDOWN;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        key = SDLK_UP;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        key = SDLK_DOWN;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        key = SDLK_LEFT;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        key = SDLK_RIGHT;
        break;
    default:
        return std::nullopt;
    }

    SDL_Event event{};
    event.type = button.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.type = static_cast<SDL_EventType>(event.type);
    event.key.timestamp = button.timestamp;
    event.key.which = 0;
    event.key.scancode = scancode_for_key(key);
    event.key.key = key;
    event.key.mod = SDL_KMOD_NONE;
    event.key.down = button.down;
    event.key.repeat = false;
    return event;
}

}  // namespace

void GamepadDeleter::operator()(SDL_Gamepad* gamepad) const
{
    SDL_CloseGamepad(gamepad);
}

bool GamepadInput::process_event(SDL_Event& event)
{
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        GamepadPtr gamepad(SDL_OpenGamepad(event.gdevice.which));
        if (gamepad) {
            SDL_Log("Gamepad connected: %s", SDL_GetGamepadName(gamepad.get()));
            gamepads_.push_back(std::move(gamepad));
        } else {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_INPUT,
                "Failed to open gamepad %d: %s",
                event.gdevice.which, SDL_GetError());
        }
        return false;
    }

    if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        std::erase_if(gamepads_, [&](const GamepadPtr& gamepad) {
            return SDL_GetGamepadID(gamepad.get()) == event.gdevice.which;
        });
        SDL_Log("Gamepad disconnected: %d", event.gdevice.which);
        return false;
    }

    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            right_trigger_held_ = event.gaxis.value >= TriggerThreshold;
        }
        return false;
    }

    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
        || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        const auto key_event = gamepad_button_as_key_event(event.gbutton);
        if (!key_event) {
            return false;
        }
        event = *key_event;
    }

    return true;
}

}  // namespace th2app
