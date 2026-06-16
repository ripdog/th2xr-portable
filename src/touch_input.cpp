#include "touch_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace th2 {
namespace {

constexpr float SwipeDistance = 0.12f;      // fraction of screen dimension
constexpr float SwipeMaxDurationMs = 500.0f;
constexpr float SwipeMinRatio = 1.5f;       // vertical must dominate horizontal
constexpr float TapMaxMovement = 0.03f;     // fraction of screen dimension
constexpr float TapMaxDurationMs = 250.0f;
constexpr float TwoFingerMaxDelayMs = 150.0f;
constexpr float BacklogLineThreshold = 0.1f; // fraction of screen height per line

float distance(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

float elapsed_ms(std::chrono::steady_clock::time_point start,
                 std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<float, std::milli>(end - start).count();
}

}  // namespace

TouchInput::TouchInput() = default;

int TouchInput::find_finger(SDL_FingerID id) const
{
    for (std::size_t i = 0; i < fingers_.size(); ++i) {
        if (fingers_[i].active && fingers_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TouchInput::add_finger(SDL_FingerID id, float x, float y)
{
    for (std::size_t i = 0; i < fingers_.size(); ++i) {
        if (!fingers_[i].active) {
            fingers_[i].id = id;
            fingers_[i].start_x = x;
            fingers_[i].start_y = y;
            fingers_[i].x = x;
            fingers_[i].y = y;
            fingers_[i].start_time = std::chrono::steady_clock::now();
            fingers_[i].active = true;
            if (scroll_anchor_y_ < 0.0f) {
                scroll_anchor_y_ = y;
            }
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TouchInput::remove_finger(int index)
{
    Finger& finger = fingers_[index];
    finger.active = false;
    finger.up_time = std::chrono::steady_clock::now();
    scroll_anchor_y_ = -1.0f;

    const bool had_scroll = emitted_scroll_;
    emitted_scroll_ = false;

    const int other = 1 - index;
    if (fingers_[other].active) {
        // Wait for the second finger to lift before deciding if this was a
        // two-finger tap.
        return;
    }

    // If we already emitted scroll events during this drag, don't also treat
    // the lift as a swipe.
    if (had_scroll) {
        active_gesture_ = false;
        return;
    }

    // If both fingers lifted close together and both moved very little,
    // treat it as a two-finger tap.
    if (fingers_[other].up_time > finger.start_time
        && elapsed_ms(fingers_[other].up_time, finger.up_time)
               < TwoFingerMaxDelayMs) {
        const float movement = std::max(
            distance(finger.start_x, finger.start_y, finger.x, finger.y),
            distance(
                fingers_[other].start_x, fingers_[other].start_y,
                fingers_[other].x, fingers_[other].y));
        const float duration = std::max(
            elapsed_ms(finger.start_time, finger.up_time),
            elapsed_ms(fingers_[other].start_time, fingers_[other].up_time));
        if (movement < TapMaxMovement && duration < TapMaxDurationMs) {
            pending_ = TouchAction::BacklogOrHideTextbox;
            active_gesture_ = false;
            return;
        }
    }

    evaluate_single_swipe(finger);

    // If it wasn't a swipe, treat a short, still lift as a tap.
    if (pending_ == TouchAction::None) {
        const auto now = std::chrono::steady_clock::now();
        const float duration = elapsed_ms(finger.start_time, now);
        const float movement =
            distance(finger.start_x, finger.start_y, finger.x, finger.y);
        if (movement < TapMaxMovement && duration < TapMaxDurationMs) {
            pending_ = TouchAction::Tap;
            tap_x_ = finger.x;
            tap_y_ = finger.y;
            active_gesture_ = false;
        }
    }
}

void TouchInput::maybe_emit_scroll(const Finger& finger)
{
    if (scroll_anchor_y_ < 0.0f) {
        return;
    }
    const int other = 1 - find_finger(finger.id);
    if (other >= 0 && fingers_[other].active) {
        return;
    }

    const float delta = finger.y - scroll_anchor_y_;
    if (delta >= BacklogLineThreshold) {
        pending_ = TouchAction::BacklogOlder;
        scroll_anchor_y_ += BacklogLineThreshold;
        emitted_scroll_ = true;
    } else if (delta <= -BacklogLineThreshold) {
        pending_ = TouchAction::BacklogNewer;
        scroll_anchor_y_ -= BacklogLineThreshold;
        emitted_scroll_ = true;
    }
}

void TouchInput::evaluate_single_swipe(const Finger& finger)
{
    const auto now = std::chrono::steady_clock::now();
    const float duration = elapsed_ms(finger.start_time, now);
    if (duration > SwipeMaxDurationMs) {
        active_gesture_ = false;
        return;
    }

    const float dx = finger.x - finger.start_x;
    const float dy = finger.y - finger.start_y;
    if (std::abs(dy) < SwipeDistance) {
        active_gesture_ = false;
        return;
    }
    if (std::abs(dy) < SwipeMinRatio * std::abs(dx)) {
        active_gesture_ = false;
        return;
    }

    active_gesture_ = false;
    if (dy > 0.0f) {
        pending_ = TouchAction::BacklogOlder;
    } else {
        pending_ = TouchAction::BacklogNewer;
    }
}

void TouchInput::process_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        const int index = add_finger(
            event.tfinger.fingerID,
            event.tfinger.x, event.tfinger.y);
        if (index >= 0) {
            active_gesture_ = true;
        }
        break;
    }
    case SDL_EVENT_FINGER_MOTION: {
        const int index = find_finger(event.tfinger.fingerID);
        if (index >= 0) {
            fingers_[index].x = event.tfinger.x;
            fingers_[index].y = event.tfinger.y;
            maybe_emit_scroll(fingers_[index]);
        }
        break;
    }
    case SDL_EVENT_FINGER_UP: {
        const int index = find_finger(event.tfinger.fingerID);
        if (index >= 0) {
            fingers_[index].x = event.tfinger.x;
            fingers_[index].y = event.tfinger.y;
            remove_finger(index);
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_AC_BACK) {
            pending_ = TouchAction::MenuToggle;
        }
        break;
    default:
        break;
    }
}

TouchAction TouchInput::poll_action()
{
    const auto action = pending_;
    pending_ = TouchAction::None;
    return action;
}

bool TouchInput::active_gesture() const
{
    return active_gesture_;
}

}  // namespace th2
