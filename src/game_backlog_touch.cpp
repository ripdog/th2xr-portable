#include "game.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#define localtime_r(timep, result) localtime_s(result, timep)
#endif

namespace th2app {

void Game::open_backlog()
{
    if (choosing_) return;
    play_se(-1, 9012, false, 140);
    ui_mode_ = UiMode::backlog;
    backlog_voice_hover_ = -1;
    backlog_depth_ = std::min(
        1, static_cast<int>(backlog_.size()));
}

void Game::close_backlog()
{
    backlog_depth_ = 0;
    backlog_voice_hover_ = -1;
    ui_mode_ = UiMode::game;
}

void Game::handle_touch_actions()
{
    using Action = th2::TouchAction;
    const auto action = touch_input_.poll_action();
    if (action == Action::None) {
        return;
    }

    switch (action) {
    case Action::BacklogOlder:
        if (ui_mode_ == UiMode::backlog) {
            backlog_older();
        } else if (ui_mode_ == UiMode::game) {
            open_backlog();
        }
        break;
    case Action::BacklogNewer:
        if (ui_mode_ == UiMode::backlog) {
            backlog_newer();
        }
        break;
    case Action::BacklogOrHideTextbox:
        if (ui_mode_ == UiMode::backlog) {
            close_backlog();
        } else if (ui_mode_ == UiMode::game) {
            message_visible_ = false;
        }
        break;
    case Action::MenuToggle:
        if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
            close_save_load();
        } else if (ui_mode_ == UiMode::system_menu) {
            close_system_menu();
        } else if (ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog) {
            open_system_menu();
        } else if (config_open_) {
            close_config();
        }
        break;
    case Action::SkipToggle:
        if (ui_mode_ == UiMode::game) {
            skip_mode_ = !skip_mode_;
            if (skip_mode_) {
                auto_mode_ = false;
            }
            play_se(-1, 9104, false, 255);
        }
        break;
    case Action::Tap: {
        // Re-inject the tap as a full left-button click.  Many handlers
        // (movie skip, menus, title) act on mouse-down, while the normal
        // text-advance handler acts on mouse-up.  Coordinates are
        // normalized, so convert to window pixels; the event loop maps
        // them to logical 800x600 coordinates on the next frame.
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_, &window_width, &window_height);
        const auto [logical_x, logical_y] = logical_coordinates(
            touch_input_.tap_x() * window_width,
            touch_input_.tap_y() * window_height,
            window_width, window_height);
        // Sidebar buttons need to work on touch too, but we must not
        // double-handle them for desktop mice (they fire on mouse-down).
        if (handle_sidebar_click(logical_x, logical_y)) {
            break;
        }
        SDL_Event down{};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.clicks = 1;
        down.button.which = SDL_TOUCH_MOUSEID;
        down.button.x = touch_input_.tap_x() * window_width;
        down.button.y = touch_input_.tap_y() * window_height;
        SDL_PushEvent(&down);
        SDL_Event up{};
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.button = SDL_BUTTON_LEFT;
        up.button.clicks = 1;
        up.button.which = SDL_TOUCH_MOUSEID;
        up.button.x = down.button.x;
        up.button.y = down.button.y;
        SDL_PushEvent(&up);
        break;
    }
    default:
        break;
    }
}

bool Game::backlog_older()
{
    if (backlog_depth_ >= static_cast<int>(backlog_.size())) {
        return false;
    }
    play_se(-1, 9012, false, 140);
    ++backlog_depth_;
    ui_mode_ = UiMode::backlog;
    return true;
}

bool Game::backlog_newer()
{
    if (backlog_depth_ <= 0) {
        return false;
    }
    play_se(-1, 9012, false, 140);
    if (backlog_depth_ > 1) {
        --backlog_depth_;
    } else {
        close_backlog();
    }
    return true;
}

void Game::execute_menu_item(int index)
{
    switch (index) {
    case 0:
        if (!replay_mode_) open_save_load(UiMode::save);
        break;
    case 1:
        if (!replay_mode_) open_save_load(UiMode::load);
        break;
    case 2: message_visible_ = !message_visible_; break;
    case 3: open_config(); break;
    case 4: break;
    }
}

void Game::open_save_load(UiMode mode)
{
    if (!save_snapshot_) {
        save_snapshot_ = capture_frame_pixels();
    }
    save_return_mode_ =
        ui_mode_ == UiMode::title ? UiMode::title : UiMode::game;
    begin_transition(1, 12, 128, false);
    ui_mode_ = mode;
    save_confirm_slot_ = -1;
    save_hover_ = -1;
    load_error_.clear();
    refresh_save_page();
    if (newest_save_slot_ >= 0) {
        save_page_ = newest_save_slot_ >= 100
            ? 10 : newest_save_slot_ / 10;
        refresh_save_page();
    }
    ensure_save_load_focus();
}

void Game::close_save_load()
{
    save_confirm_slot_ = -1;
    load_error_.clear();
    begin_transition(1, 12, 128, false);
    ui_mode_ = save_return_mode_;
}

void Game::draw_save_digit_sheet_text(
    float x, float y, std::string_view text,
    std::uint8_t red, std::uint8_t green,
    std::uint8_t blue)
{
    if (!ui_save_digits_) {
        return;
    }
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(ui_save_digits_.get(), &texture_width, &texture_height);
    const float glyph_width = texture_width / 16.0f;
    const float glyph_height = texture_height / 4.0f;
    SDL_SetTextureColorMod(ui_save_digits_.get(), red, green, blue);
    for (const unsigned char character : text) {
        if (character >= '!' && character <= '_') {
            const int index = static_cast<int>(character) - ('!' - 1);
            const SDL_FRect src{
                glyph_width * static_cast<float>(index % 16),
                glyph_height * static_cast<float>(index / 16),
                glyph_width, glyph_height};
            const SDL_FRect dst{x, y, glyph_width, glyph_height};
            SDL_RenderTexture(
                renderer_, ui_save_digits_.get(), &src, &dst);
        }
        x += glyph_width;
    }
    SDL_SetTextureColorMod(ui_save_digits_.get(), 255, 255, 255);
}

void Game::draw_save_digit_number(float x, float y, int number, int digits)
{
    if (!ui_save_digits_) {
        return;
    }
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(ui_save_digits_.get(), &texture_width, &texture_height);
    const float glyph_width = texture_width / 16.0f;
    const float glyph_height = texture_height / 4.0f;
    const auto text = std::format("{:0{}d}", number, digits);
    for (const unsigned char character : text) {
        if (character >= '0' && character <= '9') {
            const SDL_FRect src{
                glyph_width * static_cast<float>(character - '0'),
                glyph_height, glyph_width, glyph_height};
            const SDL_FRect dst{x, y, glyph_width, glyph_height};
            SDL_RenderTexture(
                renderer_, ui_save_digits_.get(), &src, &dst);
        }
        x += glyph_width;
    }
}

void Game::draw_system_menu()
{
    // Background
    if (ui_sys_menu_bg_) {
        SDL_RenderTexture(renderer_, ui_sys_menu_bg_.get(),
                          nullptr, nullptr);
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(renderer_, nullptr);
    }

    draw_save_digit_sheet_text(
        138.0f, 12.0f,
        std::format("{}A{}B", runtime_.flag(0), runtime_.flag(1)));

    // 4 main buttons from sys0110.tga
    // Layout: Save(0,0) Load(400,0) Hide(0,246) Settings(400,246)
    // Each: w=400, h=82, 3 states stacked vertically (0,82,164)
    const int btn_x[4] = {0, 400, 0, 400};
    const int btn_y[4] = {0, 0, 246, 246};
    const int dst_x[4] = {200, 200, 200, 200};
    const int dst_y[4] = {112, 200, 288, 376};

    for (int i = 0; i < 4; ++i) {
        const bool disabled = replay_mode_ && (i == 0 || i == 1);
        const int state = (i == menu_highlight_) ? 82 : 0;
        const SDL_FRect src{
            static_cast<float>(btn_x[i]),
            static_cast<float>(btn_y[i] + state), 400.0f, 82.0f};
        const SDL_FRect dst{
            static_cast<float>(dst_x[i]),
            static_cast<float>(dst_y[i]), 400.0f, 82.0f};
        if (ui_sys_menu_btns_) {
            SDL_SetTextureAlphaMod(
                ui_sys_menu_btns_.get(), disabled ? 64 : 255);
            SDL_RenderTexture(renderer_, ui_sys_menu_btns_.get(),
                              &src, &dst);
            SDL_SetTextureAlphaMod(ui_sys_menu_btns_.get(), 255);
        } else {
            // Fallback: draw text
            const char* labels[4] = {"Save", "Load", "Hide Text", "Settings"};
            const float tw = std::strlen(labels[i]) * 12.0f;
            const float tx = dst_x[i] + (400.0f - tw) / 2.0f;
            const float ty = dst_y[i] + (82.0f - 24.0f) / 2.0f;
            font_.draw(renderer_, tx + 2, ty + 2, labels[i], 0, 0, 0);
            if (i == menu_highlight_) {
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 40);
                SDL_RenderFillRect(renderer_, &dst);
                font_.draw(renderer_, tx, ty, labels[i], 255, 255, 255);
            } else {
                font_.draw(renderer_, tx, ty, labels[i], 128, 128, 128);
            }
        }
    }

    // sys0111.tga stores normal, hover and pressed states horizontally.
    const float cs = menu_highlight_ == 4 ? 188.0f : 0.0f;
    const SDL_FRect csrc{cs, 0.0f, 188.0f, 32.0f};
    const SDL_FRect cdst{306.0f, 480.0f, 188.0f, 32.0f};
    if (ui_sys_cancel_) {
        SDL_RenderTexture(renderer_, ui_sys_cancel_.get(), &csrc, &cdst);
    } else {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        if (menu_highlight_ == 4) {
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 40);
            SDL_RenderFillRect(renderer_, &cdst);
        }
        font_.draw(renderer_, 356.0f, 484.0f, "Close", 0, 0, 0);
        font_.draw(renderer_, 354.0f, 482.0f, "Close",
                   menu_highlight_ == 4 ? 255 : 128,
                   menu_highlight_ == 4 ? 255 : 128,
                   menu_highlight_ == 4 ? 255 : 128);
    }
}

void Game::draw_map_layer(
    int field, float x, float alpha,
    bool draw_field, bool draw_events)
{
    if (field < 0 || field >= 5 || !map_fields_[field]) {
        return;
    }
    if (draw_field) {
        SDL_SetTextureAlphaModFloat(map_fields_[field].get(), alpha);
        const SDL_FRect field_dst{x + 80.0f, 76.0f, 640.0f, 480.0f};
        SDL_RenderTexture(
            renderer_, map_fields_[field].get(), nullptr, &field_dst);
        SDL_SetTextureAlphaModFloat(map_fields_[field].get(), 1.0f);
    }
    if (!draw_events) {
        return;
    }

    std::array<int, 10> overlaps{};
    for (std::size_t i = 0; i < map_events_.size(); ++i) {
        const auto& event = map_events_[i];
        if (event.position < 0
            || event.position >= static_cast<int>(map_positions_.size())) {
            continue;
        }
        const auto& position = map_positions_[event.position];
        const int overlap = ++overlaps[position.overlap];
        int cx = position.x;
        int cy = position.y;
        if (overlap == 2) cx -= 200;
        else if (overlap == 3) cx += 200;
        else if (overlap == 4) { cx -= 100; cy += 160; }
        if (position.field != field) {
            continue;
        }

        if (map_markers_) {
            int state = static_cast<int>(i) == map_hover_ ? 1 : 0;
            if (static_cast<int>(i) == map_selected_) state = 2;
            const SDL_FRect src{
                static_cast<float>(state * 130),
                static_cast<float>(event.position * 118),
                130.0f, 118.0f};
            const SDL_FRect dst{
                x + cx + 20.0f, static_cast<float>(cy - 118),
                130.0f, 118.0f};
            SDL_SetTextureAlphaModFloat(map_markers_.get(), alpha);
            SDL_RenderTexture(
                renderer_, map_markers_.get(), &src, &dst);
            SDL_SetTextureAlphaModFloat(map_markers_.get(), 1.0f);
        }
        if (i < map_characters_.size()
            && map_characters_[i].texture) {
            const auto& character = map_characters_[i];
            const auto elapsed_ticks =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - map_started_)
                    .count() * 60 / 1000;
            int cycle_ticks = 0;
            for (const auto& step : character.steps) {
                cycle_ticks += step.ticks;
            }
            int tick = cycle_ticks > 0
                ? static_cast<int>(elapsed_ticks % cycle_ticks) : 0;
            int frame = character.steps.front().frame;
            for (const auto& step : character.steps) {
                if (tick < step.ticks) {
                    frame = step.frame;
                    break;
                }
                tick -= step.ticks;
            }
            if (frame < 0
                || frame >= static_cast<int>(character.frames.size())) {
                continue;
            }
            SDL_SetTextureAlphaModFloat(
                character.texture.get(), alpha);
            for (const auto& part : character.frames[frame]) {
                const SDL_FRect destination{
                    x + cx + part.x, cy + part.y,
                    part.source.w, part.source.h};
                SDL_RenderTexture(
                    renderer_, character.texture.get(),
                    &part.source, &destination);
            }
            SDL_SetTextureAlphaModFloat(
                character.texture.get(), 1.0f);
        }
    }
}

void Game::draw_map(bool ui)
{
    const float fade = map_fade_ticks_ > 0
        ? map_fade_ticks_ / 16.0f
        : std::min(1.0f,
                std::chrono::duration<float>(
                std::chrono::steady_clock::now() - map_started_).count()
                * 60.0f / 16.0f);
    if (!ui && map_frame_) {
        SDL_SetTextureAlphaModFloat(map_frame_.get(), fade);
        SDL_RenderTexture(renderer_, map_frame_.get(), nullptr, nullptr);
        SDL_SetTextureAlphaModFloat(map_frame_.get(), 1.0f);
    }

    if (map_slide_ticks_ == 0) {
        draw_map_layer(map_field_, 0.0f, fade, !ui, ui);
    } else {
        const int ticks = std::abs(map_slide_ticks_);
        const float square = static_cast<float>(ticks * ticks);
        const float direction = map_slide_ticks_ > 0 ? 1.0f : -1.0f;
        const float next_x = direction * 800.0f * square / 256.0f;
        const float previous_x =
            direction * 800.0f * (square - 256.0f) / 256.0f;
        draw_map_layer(map_field_, next_x, fade, !ui, ui);
        draw_map_layer(
            map_previous_field_, previous_x, fade, !ui, ui);
    }

    if (ui && map_arrows_) {
        const int left_state = map_hover_ == -2 ? 1 : 0;
        const int right_state = map_hover_ == -3 ? 1 : 0;
        const SDL_FRect left_src{
            static_cast<float>(left_state * 112), 0.0f, 56.0f, 122.0f};
        const SDL_FRect right_src{
            static_cast<float>(right_state * 112 + 56), 0.0f,
            56.0f, 122.0f};
        const SDL_FRect left_dst{24.0f, 239.0f, 56.0f, 122.0f};
        const SDL_FRect right_dst{720.0f, 239.0f, 56.0f, 122.0f};
        SDL_SetTextureAlphaModFloat(map_arrows_.get(), fade);
        SDL_RenderTexture(
            renderer_, map_arrows_.get(), &left_src, &left_dst);
        SDL_RenderTexture(
            renderer_, map_arrows_.get(), &right_src, &right_dst);
        SDL_SetTextureAlphaModFloat(map_arrows_.get(), 1.0f);
    }
}


}  // namespace th2app
