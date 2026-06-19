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

void Game::draw_save_load()
{
    auto& background = ui_mode_ == UiMode::save ? ui_save_bg_ : ui_load_bg_;
    if (background) {
        SDL_RenderTexture(renderer_, background.get(), nullptr, nullptr);
    }
    const SDL_FRect rows_dst{15.0f, 111.0f, 770.0f, 378.0f};
    if (ui_save_rows_) {
        SDL_RenderTexture(
            renderer_, ui_save_rows_.get(), nullptr, &rows_dst);
    }

    for (int i = 0; i < 10; ++i) {
        const float x = 16.0f + 390.0f * (i / 5);
        const float y = 112.0f + 76.0f * (i % 5);
        if (i == save_hover_ && ui_save_rows_hover_) {
            const SDL_FRect src{
                390.0f * (i / 5), 76.0f * (i % 5), 380.0f, 74.0f};
            const SDL_FRect dst{x, y, 380.0f, 74.0f};
            SDL_RenderTexture(
                renderer_, ui_save_rows_hover_.get(), &src, &dst);
        }
        if (save_thumbnails_[i]) {
            const SDL_FRect thumb{x + 15.0f, y + 5.0f, 80.0f, 60.0f};
            SDL_RenderTexture(
                renderer_, save_thumbnails_[i].get(), nullptr, &thumb);
        }

        const int slot = save_page_ * 10 + i;
        draw_save_digit_number(x + 98.0f, y + 10.0f, slot + 1, 3);
        if (!visible_saves_[i].exists) {
            continue;
        }

        std::tm local{};
        localtime_r(&visible_saves_[i].timestamp, &local);
        const auto game_date = visible_saves_[i].game_month == 0
            ? std::string("?A?B")
            : std::format(
                "{}A{}B", visible_saves_[i].game_month,
                visible_saves_[i].game_day);
        draw_save_digit_sheet_text(x + 152.0f, y + 10.0f, game_date);
        draw_save_digit_sheet_text(
            x + 98.0f, y + 43.0f,
            std::format("{:04d}", local.tm_year + 1900), 120, 43, 56);
        draw_save_digit_sheet_text(
            x + 164.0f, y + 43.0f,
            std::format("{:02d}/{:02d}", local.tm_mon + 1, local.tm_mday),
            120, 43, 56);
        draw_save_digit_sheet_text(
            x + 244.0f, y + 43.0f,
            std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min),
            120, 43, 56);
        font_.draw_save_menu(
            renderer_, x + 222.0f, y + 10.0f,
            visible_saves_[i].message.substr(0, 18), 255, 245, 225);
        if (slot == newest_save_slot_ && ui_save_new_) {
            const SDL_FRect badge{x + 316.0f, y + 37.0f, 56.0f, 29.0f};
            SDL_RenderTexture(
                renderer_, ui_save_new_.get(), nullptr, &badge);
        }
    }

    constexpr int total_pages = 11;
    draw_save_digit_sheet_text(
        364.0f, 78.0f,
        std::format("{:02d}/{:02d}", save_page_ + 1, total_pages));
    if (ui_save_controls_) {
        const SDL_FRect prev_src{
            0.0f, save_hover_ == 10 ? 64.0f : 0.0f, 130.0f, 32.0f};
        const SDL_FRect next_src{
            0.0f, 32.0f + (save_hover_ == 11 ? 64.0f : 0.0f),
            130.0f, 32.0f};
        const SDL_FRect prev_dst{190.0f, 72.0f, 130.0f, 32.0f};
        const SDL_FRect next_dst{482.0f, 72.0f, 130.0f, 32.0f};
        SDL_RenderTexture(
            renderer_, ui_save_controls_.get(), &prev_src,
            &prev_dst);
        SDL_RenderTexture(
            renderer_, ui_save_controls_.get(), &next_src,
            &next_dst);
    }
    if (ui_sys_cancel_) {
        const SDL_FRect src{
            save_hover_ == 12 ? 188.0f : 0.0f, 0.0f, 188.0f, 32.0f};
        const SDL_FRect dst{306.0f, 496.0f, 188.0f, 32.0f};
        SDL_RenderTexture(renderer_, ui_sys_cancel_.get(), &src, &dst);
    }

    if (save_confirm_slot_ >= 0) {
        auto& prompt =
            ui_mode_ == UiMode::save ? ui_save_prompt_ : ui_load_prompt_;
        if (prompt) {
            const SDL_FRect dst{0.0f, 246.0f, 800.0f, 142.0f};
            SDL_RenderTexture(renderer_, prompt.get(), nullptr, &dst);
        }
        const int selected = save_confirm_slot_ - save_page_ * 10;
        if (selected >= 0 && selected < 10) {
            const float x = 25.0f;
            const float y = 271.0f;
            if (save_thumbnails_[selected]) {
                const SDL_FRect thumb{x, y + 2.0f, 80.0f, 60.0f};
                SDL_RenderTexture(
                    renderer_, save_thumbnails_[selected].get(),
                    nullptr, &thumb);
            }
            draw_save_digit_number(
                x + 98.0f, y + 4.0f, save_confirm_slot_ + 1, 3);
            const auto game_date =
                visible_saves_[selected].game_month == 0
                ? std::string("?A?B")
                : std::format(
                    "{}A{}B", visible_saves_[selected].game_month,
                    visible_saves_[selected].game_day);
            draw_save_digit_sheet_text(x + 152.0f, y + 4.0f, game_date);
            font_.draw_save_menu(
                renderer_, x + 222.0f, y + 4.0f,
                visible_saves_[selected].message.substr(0, 18),
                255, 245, 225);

            std::tm local{};
            localtime_r(
                &visible_saves_[selected].timestamp, &local);
            draw_save_digit_sheet_text(
                x + 98.0f, y + 37.0f,
                std::format("{:04d}", local.tm_year + 1900),
                120, 43, 56);
            draw_save_digit_sheet_text(
                x + 164.0f, y + 37.0f,
                std::format(
                    "{:02d}/{:02d}", local.tm_mon + 1, local.tm_mday),
                120, 43, 56);
            draw_save_digit_sheet_text(
                x + 244.0f, y + 37.0f,
                std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min),
                120, 43, 56);
        }
        if (ui_confirm_buttons_) {
            const SDL_FRect yes_src{
                0.0f, save_hover_ == 13 ? 32.0f : 0.0f, 130.0f, 32.0f};
            const SDL_FRect no_src{
                130.0f, save_hover_ == 14 ? 32.0f : 0.0f,
                130.0f, 32.0f};
            const SDL_FRect yes_dst{461.0f, 320.0f, 130.0f, 32.0f};
            const SDL_FRect no_dst{606.0f, 320.0f, 130.0f, 32.0f};
            SDL_RenderTexture(
                renderer_, ui_confirm_buttons_.get(), &yes_src,
                &yes_dst);
            SDL_RenderTexture(
                renderer_, ui_confirm_buttons_.get(), &no_src,
                &no_dst);
        }
    }

    if (!load_error_.empty()) {
        font_.draw_save_menu(
            renderer_, 21.0f, 571.0f, load_error_, 0, 0, 0);
        font_.draw_save_menu(
            renderer_, 20.0f, 570.0f, load_error_, 255, 80, 80);
    }
}

int Game::save_load_hit(float x, float y) const
{
    if (save_confirm_slot_ >= 0) {
        if (x >= 461 && x < 591 && y >= 320 && y < 352) return 13;
        if (x >= 606 && x < 736 && y >= 320 && y < 352) return 14;
        return -1;
    }
    for (int i = 0; i < 10; ++i) {
        const float left = 16.0f + 390.0f * (i / 5);
        const float top = 112.0f + 76.0f * (i % 5);
        if (x >= left && x < left + 380 && y >= top && y < top + 74) {
            return i;
        }
    }
    if (x >= 190 && x < 320 && y >= 72 && y < 104) return 10;
    if (x >= 482 && x < 612 && y >= 72 && y < 104) return 11;
    if (x >= 306 && x < 494 && y >= 496 && y < 528) return 12;
    return -1;
}

void Game::activate_save_load_item(int item)
{
    load_error_.clear();
    if (item >= 0 && item < 10) {
        const int slot = save_page_ * 10 + item;
        if (ui_mode_ == UiMode::load && !visible_saves_[item].exists) {
            return;
        }
        play_se(-1, 9104, false, 255);
        if (ui_mode_ == UiMode::save && !visible_saves_[item].exists) {
            save(slot);
            close_save_load();
        } else {
            save_confirm_slot_ = slot;
            save_hover_ = 14;
        }
    } else if (item == 10 || item == 11) {
        play_se(-1, 9104, false, 255);
        constexpr int page_count = 11;
        save_page_ = (save_page_ + (item == 10 ? page_count - 1 : 1)) % page_count;
        refresh_save_page();
    } else if (item == 12) {
        play_se(-1, 9104, false, 255);
        close_save_load();
    } else if (item == 13 && save_confirm_slot_ >= 0) {
        play_se(-1, 9104, false, 255);
        const int slot = save_confirm_slot_;
        if (ui_mode_ == UiMode::save) {
            save(slot);
            close_save_load();
        } else if (load(slot)) {
            save_confirm_slot_ = -1;
            ui_mode_ = UiMode::game;
        } else {
            load_error_ = "Incompatible save version.";
        }
    } else if (item == 14) {
        play_se(-1, 9104, false, 255);
        save_confirm_slot_ = -1;
        save_hover_ = -1;
    }
}

void Game::handle_save_load_input(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const int hovered = save_load_hit(event.motion.x, event.motion.y);
        if (hovered != save_hover_) {
            save_hover_ = hovered;
            if (save_hover_ >= 0) {
                play_se(-1, 9108, false, 255);
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            play_se(-1, 9107, false, 255);
            if (save_confirm_slot_ >= 0) {
                save_confirm_slot_ = -1;
            } else {
                close_save_load();
            }
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            activate_save_load_item(
                save_load_hit(event.button.x, event.button.y));
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            play_se(-1, 9107, false, 255);
            if (save_confirm_slot_ >= 0) {
                save_confirm_slot_ = -1;
            } else {
                close_save_load();
            }
        } else if (event.key.key == SDLK_PAGEUP) {
            activate_save_load_item(10);
        } else if (event.key.key == SDLK_PAGEDOWN) {
            activate_save_load_item(11);
        } else if (event.key.key == SDLK_RETURN
                   || event.key.key == SDLK_SPACE) {
            // When a save slot has been clicked and the confirmation
            // dialog is showing, save_hover_ points at the yes/no buttons
            // (13/14).  Enter/Space activates whatever save_hover_ is
            // currently set to.
            activate_save_load_item(save_hover_);
        }
    }
}

void Game::handle_system_menu_input(const SDL_Event& event)
{
    const int dst_x[5] = {200, 200, 200, 200, 306};
    const int dst_y[5] = {112, 200, 288, 376, 480};
    const int dst_w[5] = {400, 400, 400, 400, 188};
    const int dst_h[5] = {82, 82, 82, 82, 32};
    const auto enabled = [&](int item) {
        return !replay_mode_ || (item != 0 && item != 1);
    };
    const auto move_highlight = [&](int direction) {
        do {
            menu_highlight_ =
                (menu_highlight_ + direction + 5) % 5;
        } while (!enabled(menu_highlight_));
    };

    const int previous_highlight = menu_highlight_;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            play_se(-1, 9107, false, 255);
            close_system_menu();
        } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE) {
            const int item = menu_highlight_;
            if (enabled(item)) {
                play_se(-1, 9014, false, 255);
                close_system_menu();
                execute_menu_item(item);
            }
        } else if (event.key.key == SDLK_UP) {
            move_highlight(-1);
        } else if (event.key.key == SDLK_DOWN) {
            move_highlight(1);
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            play_se(-1, 9107, false, 255);
            close_system_menu();
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            const float mx = event.button.x; const float my = event.button.y;
            for (int i = 0; i < 5; ++i) {
                if (mx >= dst_x[i] && mx < dst_x[i] + dst_w[i]
                    && my >= dst_y[i] && my < dst_y[i] + dst_h[i]
                    && enabled(i)) {
                    play_se(-1, 9014, false, 255);
                    close_system_menu();
                    execute_menu_item(i);
                    break;
                }
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        const float mx = event.motion.x; const float my = event.motion.y;
        for (int i = 0; i < 5; ++i) {
            if (mx >= dst_x[i] && mx < dst_x[i] + dst_w[i]
                && my >= dst_y[i] && my < dst_y[i] + dst_h[i]
                && enabled(i)) {
                menu_highlight_ = i;
                break;
            }
        }
    }
    if (menu_highlight_ != previous_highlight) {
        play_se(-1, 9108, false, 255);
    }
}

void Game::change_map_field(int direction)
{
    if (map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
        return;
    }
    map_previous_field_ = map_field_;
    do {
        map_field_ = (map_field_ + direction + 5) % 5;
    } while (!map_fields_[map_field_]);
    map_slide_ticks_ = direction > 0 ? 16 : -16;
    map_hover_ = -1;
    play_se(-1, 9015, false, 255);
}

void Game::update_map_hover(float x, float y)
{
    map_hover_ = -1;
    if (x >= 24.0f && x < 80.0f && y >= 239.0f && y < 361.0f) {
        map_hover_ = -2;
        return;
    }
    if (x >= 720.0f && x < 776.0f && y >= 239.0f && y < 361.0f) {
        map_hover_ = -3;
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
        if (position.field == map_field_
            && x >= cx + 20 && x < cx + 150
            && y >= cy - 118 && y < cy) {
            map_hover_ = static_cast<int>(i);
            return;
        }
    }
}

void Game::handle_map_input(const SDL_Event& event)
{
    if (map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
        return;
    }
    const int previous = map_hover_;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        update_map_hover(event.motion.x, event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
               && event.button.button == SDL_BUTTON_LEFT) {
        update_map_hover(event.button.x, event.button.y);
        if (map_hover_ == -2) {
            change_map_field(1);
        } else if (map_hover_ == -3) {
            change_map_field(-1);
        } else if (map_hover_ >= 0) {
            finish_map_selection(map_hover_);
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_PAGEUP
            || event.key.key == SDLK_RIGHT) {
            change_map_field(1);
        } else if (event.key.key == SDLK_PAGEDOWN
                   || event.key.key == SDLK_LEFT) {
            change_map_field(-1);
        } else if ((event.key.key == SDLK_RETURN
                    || event.key.key == SDLK_SPACE)
                   && map_hover_ >= 0) {
            finish_map_selection(map_hover_);
        }
    }
    if (map_hover_ != previous && map_hover_ != -1) {
        play_se(-1, 9108, false, 255);
    }
}

void Game::update_map()
{
    if (ui_mode_ != UiMode::map) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    constexpr auto tick = std::chrono::microseconds(1'000'000 / 60);
    while (now - map_tick_ >= tick) {
        map_tick_ += tick;
        if (map_slide_ticks_ > 0) {
            --map_slide_ticks_;
        } else if (map_slide_ticks_ < 0) {
            ++map_slide_ticks_;
        }
        if (map_fade_ticks_ > 0) {
            --map_fade_ticks_;
            if (map_fade_ticks_ == 0) {
                complete_map_selection();
                return;
            }
        }
    }
}

void Game::draw_backlog()
{
    const bool authentic = font_.authentic();
    if (authentic) {
        begin_authentic_text();
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 128);
    const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
    SDL_RenderFillRect(renderer_, &game_area);

    std::string_view selected = message_.visible();
    const BacklogEntry* entry = nullptr;
    if (backlog_depth_ > 0 && backlog_depth_ <= static_cast<int>(backlog_.size())) {
        entry = &backlog_[
            backlog_.size() - static_cast<std::size_t>(backlog_depth_)];
        selected = entry->text;
    }

    const float x = message_text_x();
    float y = message_text_y();
    for (const auto& line : display_lines(selected)) {
        if (authentic) {
            font_.draw_authentic_shadow(renderer_, x, y, line);
        } else {
            font_.draw(renderer_, x + 2.0f, y + 2.0f, line, 0, 0, 0);
        }
        font_.draw(renderer_, x, y, line, 255, 144, 32);
        y += text_line_height();
        if (y > 535.0f) {
            break;
        }
    }
    if (entry && backlog_voice_hover_ >= 0
        && backlog_voice_hover_
            < static_cast<int>(entry->voices.size())) {
        const auto lines = display_lines(entry->text);
        for (const auto& rect :
             backlog_voice_rects(*entry, backlog_voice_hover_)) {
            const auto line = static_cast<std::size_t>(
                (rect.y - message_text_y()) / text_line_height());
            if (line >= lines.size()) {
                continue;
            }
            const SDL_Rect clip{
                static_cast<int>(std::floor(rect.x)),
                static_cast<int>(std::floor(rect.y)),
                static_cast<int>(std::ceil(rect.w)),
                static_cast<int>(std::ceil(rect.h))};
            SDL_SetRenderClipRect(renderer_, &clip);
            if (authentic) {
                font_.draw_authentic_shadow(
                    renderer_, x, rect.y, lines[line]);
            } else {
                font_.draw(
                    renderer_, x + 2.0f, rect.y + 2.0f,
                    lines[line], 0, 0, 0);
            }
            font_.draw(
                renderer_, x, rect.y, lines[line], 255, 255, 255);
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
    }

    if (authentic) {
        select_overlay();
    }
}

std::vector<SDL_FRect> Game::backlog_voice_rects(
    const Game::BacklogEntry& entry, int voice_index) const
{
    std::vector<SDL_FRect> result;
    if (voice_index < 0
        || voice_index >= static_cast<int>(entry.voices.size())) {
        return result;
    }
    const auto& voice = entry.voices[voice_index];
    const auto start = std::min(voice.start, entry.text.size());
    const auto end = std::clamp(voice.end, start, entry.text.size());
    std::size_t source_cursor = 0;
    float y = message_text_y();
    for (const auto& line : display_lines(entry.text)) {
        auto line_start =
            std::string_view(entry.text).find(line, source_cursor);
        if (line_start == std::string_view::npos) {
            line_start = source_cursor;
        }
        const auto line_end = line_start + line.size();
        const auto overlap_start = std::max(start, line_start);
        const auto overlap_end = std::min(end, line_end);
        if (overlap_start < overlap_end) {
            const auto prefix = std::string_view(line).substr(
                0, overlap_start - line_start);
            const auto voiced = std::string_view(line).substr(
                0, overlap_end - line_start);
            const float left =
                message_text_x() + font_.text_width(prefix);
            const float right =
                message_text_x() + font_.text_width(voiced);
            result.push_back(
                {left, y, right - left, text_line_height()});
        }
        source_cursor = line_end;
        y += text_line_height();
    }
    return result;
}

void Game::draw_sidebar()
{
    if (!ui_sidebar_track_ || !ui_sidebar_btns_) return;

    switch (config_.sidebar_mode) {
    case 0:
        sidebar_alpha_ = std::clamp(
            sidebar_alpha_ + (sidebar_mouse_near_ ? 24.0f : -24.0f),
            64.0f, 255.0f);
        break;
    case 1:
        sidebar_alpha_ = 255.0f;
        break;
    case 2:
        sidebar_alpha_ = std::clamp(
            sidebar_alpha_ + (sidebar_mouse_near_ ? 32.0f : -32.0f),
            0.0f, 255.0f);
        break;
    default:
        sidebar_alpha_ = 0.0f;
        break;
    }
    if (sidebar_alpha_ <= 0.0f) {
        return;
    }
    const auto alpha = static_cast<std::uint8_t>(sidebar_alpha_);
    SDL_SetTextureAlphaMod(ui_sidebar_track_.get(), alpha);
    SDL_SetTextureAlphaMod(ui_sidebar_btns_.get(), alpha);

    // sys0000.tga is the complete 30x600 sidebar backing.
    const SDL_FRect sidebar_dst{770.0f, 0.0f, 30.0f, 600.0f};
    SDL_RenderTexture(renderer_, ui_sidebar_track_.get(), nullptr,
                      &sidebar_dst);

    // sys0001.tga stores disabled, normal, hover and pressed states
    // in four 22-pixel-wide columns.
    {
        const float ratio = backlog_.empty() ? 1.0f
            : 1.0f - static_cast<float>(backlog_depth_)
                / static_cast<float>(backlog_.size());
        const float handle_y = 10.0f + ratio * (255.0f - 31.0f);
        const float handle_state = backlog_handle_dragging_ ? 3.0f
            : backlog_handle_hover_ ? 2.0f
            : backlog_.size() > 1 ? 1.0f : 0.0f;
        const SDL_FRect hdl_src{
            handle_state * 22.0f, 0.0f, 22.0f, 30.0f};
        const SDL_FRect hdl_dst{776.0f, handle_y, 22.0f, 30.0f};
        SDL_RenderTexture(renderer_, ui_sidebar_btns_.get(),
                          &hdl_src, &hdl_dst);
    }

    struct SBBtn { int y; int source_y; int h; };
    const SBBtn btns[] = {
        {271, 36, 36},   // PageUp
        {312, 77, 36},   // PageDown
        {353, 118, 20},  // Save
        {376, 141, 20},  // Load
        {399, 164, 20},  // Auto
        {422, 187, 20},  // Skip
        {445, 210, 20},  // Settings
        {468, 233, 20},  // QuickSave
    };

    for (int i = 0; i < static_cast<int>(std::size(btns)); ++i) {
        const auto& button = btns[i];
        const bool disabled = replay_mode_ && (i == 2 || i == 3);
        const bool active =
            (i == 4 && auto_mode_) || (i == 5 && skip_mode_);
        const float state_x = disabled ? 0.0f : active ? 66.0f
            : (i == sidebar_hover_ ? 44.0f : 22.0f);
        const SDL_FRect src{
            state_x, static_cast<float>(button.source_y),
            22.0f, static_cast<float>(button.h)};
        const SDL_FRect dst{
            776.0f, static_cast<float>(button.y),
            22.0f, static_cast<float>(button.h)};
        SDL_RenderTexture(renderer_, ui_sidebar_btns_.get(),
                          &src, &dst);
    }
    SDL_SetTextureAlphaMod(ui_sidebar_track_.get(), 255);
    SDL_SetTextureAlphaMod(ui_sidebar_btns_.get(), 255);
}

void Game::update_sidebar_hover(float x, float y)
{
    sidebar_mouse_near_ = x >= 776.0f;
    backlog_handle_hover_ = false;
    const int previous_hover = sidebar_hover_;
    sidebar_hover_ = -1;
    if (config_.sidebar_mode == 3) {
        return;
    }
    if (x < 776.0f || x >= 798.0f) {
        return;
    }
    if (!backlog_.empty()) {
        const float ratio = backlog_.empty() ? 1.0f
            : 1.0f - static_cast<float>(backlog_depth_)
                / static_cast<float>(backlog_.size());
        const float handle_y = 10.0f + ratio * (255.0f - 31.0f);
        backlog_handle_hover_ =
            y >= handle_y && y < handle_y + 30.0f;
    }
    static constexpr std::array button_y{
        271, 312, 353, 376, 399, 422, 445, 468,
    };
    static constexpr std::array button_h{
        36, 36, 20, 20, 20, 20, 20, 20,
    };
    for (int i = 0; i < static_cast<int>(button_y.size()); ++i) {
        if (y >= button_y[i] && y < button_y[i] + button_h[i]) {
            if (replay_mode_ && (i == 2 || i == 3)) {
                return;
            }
            sidebar_hover_ = i;
            if (sidebar_hover_ != previous_hover) {
                play_se(-1, 9108, false, 255);
            }
            return;
        }
    }
}

bool Game::handle_sidebar_click(float x, float y)
{
    if (config_.sidebar_mode == 3) {
        return false;
    }
    if (x < 776.0f || x >= 798.0f) {
        return false;
    }
    if (y >= 10.0f && y < 265.0f && !backlog_.empty()) {
        backlog_handle_dragging_ = true;
        set_backlog_from_sidebar_y(y);
        return true;
    }

    struct Hitbox { int y; int h; };
    static constexpr Hitbox buttons[] = {
        {271, 36}, {312, 36}, {353, 20}, {376, 20},
        {399, 20}, {422, 20}, {445, 20}, {468, 20},
    };
    for (int i = 0; i < static_cast<int>(std::size(buttons)); ++i) {
        if (y < buttons[i].y || y >= buttons[i].y + buttons[i].h) {
            continue;
        }
        switch (i) {
        case 0:
            backlog_older();
            break;
        case 1:
            if (!backlog_newer()) {
                advance();
            }
            break;
        case 2:
            if (replay_mode_) break;
            play_se(-1, 9104, false, 255);
            save_snapshot_ = capture_frame_pixels();
            open_save_load(UiMode::save);
            break;
        case 3:
            if (replay_mode_) break;
            play_se(-1, 9104, false, 255);
            save_snapshot_ = capture_frame_pixels();
            open_save_load(UiMode::load);
            break;
        case 4:
            play_se(-1, 9104, false, 255);
            auto_mode_ = !auto_mode_;
            if (auto_mode_) skip_mode_ = false;
            break;
        case 5:
            play_se(-1, 9104, false, 255);
            skip_mode_ = !skip_mode_;
            if (skip_mode_) auto_mode_ = false;
            break;
        case 6:
            play_se(-1, 9104, false, 255);
            message_visible_ = !message_visible_;
            break;
        case 7:
            play_se(-1, 9104, false, 255);
            open_config();
            break;
        }
        return true;
    }
    return true;
}

void Game::set_backlog_from_sidebar_y(float y)
{
    if (backlog_.empty()) {
        backlog_depth_ = 0;
        ui_mode_ = UiMode::game;
        return;
    }
    constexpr float track_top = 10.0f;
    constexpr float handle_height = 30.0f;
    constexpr float track_height = 255.0f;
    const float handle_y = std::clamp(
        y - handle_height / 2.0f,
        track_top, track_top + track_height - handle_height);
    const float ratio =
        (handle_y - track_top) / (track_height - handle_height);
    backlog_depth_ = std::clamp(
        static_cast<int>(std::lround(
            (1.0f - ratio) * static_cast<float>(backlog_.size()))),
        0, static_cast<int>(backlog_.size()));
    backlog_voice_hover_ = -1;
    ui_mode_ = backlog_depth_ == 0 ? UiMode::game : UiMode::backlog;
}

void Game::handle_backlog_input(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            close_backlog();
        } else if (event.key.key == SDLK_UP) {
            backlog_older();
        } else if (event.key.key == SDLK_DOWN) {
            backlog_newer();
        } else if (event.key.key == SDLK_PAGEUP) {
            backlog_older();
        } else if (event.key.key == SDLK_PAGEDOWN) {
            backlog_newer();
        } else if (event.key.key == SDLK_HOME) {
            if (backlog_depth_ != static_cast<int>(backlog_.size())) {
                play_se(-1, 9012, false, 140);
                backlog_depth_ = static_cast<int>(backlog_.size());
            }
        } else if (event.key.key == SDLK_END) {
            close_backlog();
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            close_backlog();
            return;
        }
        if (event.button.which != SDL_TOUCH_MOUSEID
            && handle_sidebar_click(event.button.x, event.button.y)) {
            return;
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (backlog_voice_hover_ >= 0
                && backlog_depth_ > 0
                && backlog_depth_
                    <= static_cast<int>(backlog_.size())) {
                const auto& entry = backlog_[
                    backlog_.size()
                    - static_cast<std::size_t>(backlog_depth_)];
                if (backlog_voice_hover_
                    < static_cast<int>(entry.voices.size())) {
                    replay_backlog_voice(
                        entry.voices[backlog_voice_hover_]);
                    return;
                }
            }
            close_backlog();
        }
        backlog_handle_dragging_ = false;
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (event.wheel.y > 0) {
            backlog_older();
        } else if (event.wheel.y < 0) {
            backlog_newer();
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (backlog_handle_dragging_) {
            set_backlog_from_sidebar_y(event.motion.y);
            return;
        }
        update_sidebar_hover(event.motion.x, event.motion.y);
        backlog_voice_hover_ = -1;
        if (event.motion.x < 770.0f && backlog_depth_ > 0
            && backlog_depth_
                <= static_cast<int>(backlog_.size())) {
            const auto& entry = backlog_[
                backlog_.size()
                - static_cast<std::size_t>(backlog_depth_)];
            for (int i = 0;
                 i < static_cast<int>(entry.voices.size()); ++i) {
                for (const auto& rect :
                     backlog_voice_rects(entry, i)) {
                    if (event.motion.x >= rect.x
                        && event.motion.x < rect.x + rect.w
                        && event.motion.y >= rect.y
                        && event.motion.y < rect.y + rect.h) {
                        backlog_voice_hover_ = i;
                        break;
                    }
                }
                if (backlog_voice_hover_ >= 0) {
                    break;
                }
            }
        }
    }
}


}  // namespace th2app
