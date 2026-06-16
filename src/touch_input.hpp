#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <chrono>

namespace th2 {

enum class TouchAction {
    None,
    // Swipe down: open backlog in-game, or scroll to older entries if already
    // in the backlog.
    BacklogOlder,
    // Swipe up: scroll to newer backlog entries, or close the backlog when at
    // the newest entry.
    BacklogNewer,
    // Two-finger tap: close backlog if open, otherwise hide the message
    // textbox.
    BacklogOrHideTextbox,
    // Android back button: toggle the system menu in-game, or close the
    // save/load menus.
    MenuToggle,
    // Single-finger tap.  Coordinates are stored separately and should be
    // converted to a left mouse-button-up event so the normal UI handlers run.
    Tap,
};

// Interprets touch and Android back-button events as high-level game actions.
// Single-finger taps are left untouched so SDL's touch-to-mouse synthesis
// still advances text normally.
class TouchInput {
public:
    TouchInput();
    ~TouchInput() = default;

    TouchInput(const TouchInput&) = delete;
    TouchInput& operator=(const TouchInput&) = delete;

    void process_event(const SDL_Event& event);

    // Returns the most recently recognized action and clears it.
    TouchAction poll_action();

    // Normalized coordinates (0..1) of the last recognized tap.  Only valid
    // when poll_action() returns TouchAction::Tap.
    float tap_x() const { return tap_x_; }
    float tap_y() const { return tap_y_; }

    // True while a gesture is in progress.
    bool active_gesture() const;

private:
    struct Finger {
        SDL_FingerID id = 0;
        float start_x = 0.0f;
        float start_y = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point up_time;
        bool active = false;
    };

    int find_finger(SDL_FingerID id) const;
    int add_finger(SDL_FingerID id, float x, float y);
    void remove_finger(int index);
    void evaluate_single_swipe(const Finger& finger);
    void evaluate_two_finger_tap();
    void maybe_emit_scroll(const Finger& finger);

    std::array<Finger, 2> fingers_{};
    TouchAction pending_ = TouchAction::None;
    bool active_gesture_ = false;
    bool emitted_scroll_ = false;
    float scroll_anchor_y_ = -1.0f;
    float tap_x_ = 0.0f;
    float tap_y_ = 0.0f;
};

}  // namespace th2
