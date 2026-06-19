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

void Game::load_script(std::string name)
{
    runtime_.load(std::move(name));
    vi_event_voice_no_ = -1;
    vi_event_voice_no_all_ = -1;
}

bool Game::load_scheduled_script()
{
    constexpr std::size_t month_flag = 0;
    constexpr std::size_t day_flag = 1;
    constexpr std::size_t time_flag = 2;
    constexpr std::size_t event_next_flag = 3;
    constexpr std::size_t event_end_flag = 4;
    constexpr std::size_t calendar_skip_flag = 6;
    constexpr std::size_t clock_time_flag = 7;

    int month = runtime_.flag(month_flag);
    int day = runtime_.flag(day_flag);
    int time = runtime_.flag(time_flag);
    const int event_next = runtime_.flag(event_next_flag);
    bool show_calendar = false;

    if (runtime_.flag(event_end_flag) != 0) {
        time = event_next == -1 ? time + 1 : event_next;
        if (time >= 7) {
            map_events_.clear();
            if (skipped_month_ != 0) {
                month = skipped_month_;
                day = skipped_day_;
                skipped_month_ = 0;
                skipped_day_ = 0;
            } else {
                ++day;
            }
            time = 0;
            runtime_.set_flag(clock_time_flag, 0);
            if (runtime_.flag(calendar_skip_flag) != 0) {
                runtime_.set_flag(calendar_skip_flag, 0);
            } else {
                show_calendar = true;
            }
            if ((month == 3 && day >= 32)
                || (month == 4 && day >= 31)) {
                ++month;
                day = 1;
            }
        }
    }

    const int weekday = month == 3 ? day % 7
        : month == 4 ? (day + 3) % 7
        : (day + 5) % 7;
    const bool holiday =
        (month == 3 && (day == 20 || day >= 25))
        || (month == 4 && (day <= 7 || day == 29))
        || (month == 5 && (day == 3 || day == 4 || day == 5));
    if ((weekday == 0 || holiday) && time == 5) {
        time = 6;
    }

    if (time == 5) {
        runtime_.set_flag(month_flag, month);
        runtime_.set_flag(day_flag, day);
        runtime_.set_flag(time_flag, time);
        begin_map();
        return true;
    }
    map_events_.clear();

    static constexpr std::array periods{
        "MORNING", "INTERVAL", "LUNCH_BREAK", "SCHOOL_HOURS",
        "AFTER_SCHOOL", "", "NIGHT",
    };
    if (time < 0 || time >= static_cast<int>(periods.size())
        || periods[time][0] == '\0') {
        return false;
    }

    runtime_.set_flag(month_flag, month);
    runtime_.set_flag(day_flag, day);
    runtime_.set_flag(time_flag, time);
    runtime_.set_flag(event_end_flag, 0);
    runtime_.set_flag(event_next_flag, -1);
    load_script(std::format(
        "EV_{:02d}{:02d}{}.SDT", month, day, periods[time]));
    if (show_calendar) {
        begin_calendar(-1, -1);
    }
    return true;
}

bool Game::voice_playing() const
{
    return std::any_of(
        voice_channels_.begin(), voice_channels_.end(),
        [](const th2::AudioChannel& channel) {
            return channel.playing();
        });
}

void Game::update_playback_modes()
{
    if (config_open_ || ui_mode_ != UiMode::game || choosing_
        || transition_ || background_fade_ || screen_flash_
        || (shake_ && shake_->frames > 0)
        || background_scroll_ || character_animation_active()
        || clock_state_ || calendar_state_
        || wake_time_ || audio_wait_) {
        auto_next_time_.reset();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (skip_mode_) {
        if (now >= skip_next_time_
            && (config_.skip_unread || current_text_is_read())) {
            skip();
            skip_next_time_ = now + std::chrono::milliseconds(40);
        }
        return;
    }
    if ((!auto_mode_ && !demo_mode_)
        || !waiting_for_input_ || !text_reveal_complete_
        || voice_playing()) {
        auto_next_time_.reset();
        return;
    }
    if (!auto_next_time_) {
        const int delay = demo_mode_
            ? std::max(0, demo_delay_frames_) * 1000 / 60
            : th2::auto_delay_ms(
                config_, current_text_is_read(),
                message_.has_hidden_segments(), message_ends_block_);
        auto_next_time_ = now + std::chrono::milliseconds(delay);
    } else if (now >= *auto_next_time_) {
        auto_next_time_.reset();
        advance();
    }
}

float Game::choice_y_start() const
{
    if (!message_.empty()) {
        return message_text_y()
            + static_cast<float>(display_lines(message_.visible()).size())
                * text_line_height()
            + 1.0f;
    }
    return 468.0f;
}

float Game::choice_height(const Choice& choice) const
{
    return std::max<std::size_t>(
        1, display_lines(choice.text).size()) * text_line_height();
}

std::vector<std::string> Game::choice_lines(
    const Choice& choice, int index) const
{
    auto lines = display_lines(choice.text);
    if (!lines.empty()) {
        lines.front() = std::format("{}. {}", index + 1, lines.front());
    }
    return lines;
}

std::size_t Game::text_wrap_columns() const
{
    if (font_.authentic()) {
        return 60;
    }
    return static_cast<std::size_t>(std::clamp(
        60 * 24 / std::max(config_.font_size, 1), 30, 80));
}

float Game::text_line_height() const
{
    if (font_.authentic()) {
        return 31.0f;
    }
    return static_cast<float>(std::max(31, config_.font_size + 7));
}

std::vector<std::string> Game::display_lines(std::string_view source) const
{
    return th2app::display_lines(source, text_wrap_columns());
}

float Game::message_text_x() const
{
    return 26.0f;
}

float Game::message_text_y() const
{
    return 36.0f;
}

std::size_t Game::utf8_prefix_bytes(
    std::string_view text, std::size_t characters)
{
    std::size_t position = 0;
    while (position < text.size() && characters > 0) {
        const auto byte = static_cast<unsigned char>(text[position]);
        position += byte < 0x80 ? 1
            : byte < 0xe0 ? 2
            : byte < 0xf0 ? 3
            : 4;
        --characters;
    }
    return std::min(position, text.size());
}

std::size_t Game::utf8_character_count(std::string_view text)
{
    return std::count_if(
        text.begin(), text.end(), [](unsigned char byte) {
            return (byte & 0xc0) != 0x80;
        });
}

void Game::start_text_reveal(std::size_t start)
{
    text_reveal_start_ = start;
    text_reveal_started_ = std::chrono::steady_clock::now();
    text_reveal_complete_ = config_.text_speed_ms == 0;
}

bool Game::finish_text_reveal()
{
    if (text_reveal_complete_) {
        return false;
    }
    text_reveal_complete_ = true;
    return true;
}

void Game::skip(bool force_unread)
{
    if (clock_state_) {
        if (force_unread) {
            clock_state_->started -= std::chrono::milliseconds(250);
        }
        return;
    }
    if (calendar_state_) {
        if (!calendar_state_->dismissing) {
            calendar_state_->dismissing = true;
            calendar_state_->started = std::chrono::steady_clock::now();
        } else if (force_unread) {
            calendar_state_->started -= std::chrono::milliseconds(250);
        }
        return;
    }
    if (choosing_) {
        return;
    }
    if (transition_) {
        if (force_unread) {
            transition_->started = std::chrono::steady_clock::now()
                - std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        transition_->frames / 60.0));
        }
        return;
    }
    if (background_fade_) {
        if (force_unread) {
            background_fade_->started -= background_fade_->duration;
        }
        return;
    }
    if (screen_flash_) {
        if (force_unread) {
            const auto frames = screen_flash_->fade_in_frames
                + screen_flash_->fade_out_frames;
            screen_flash_->started -= std::chrono::milliseconds(
                frames * 1000 / 60);
        }
        return;
    }
    if (shake_ && shake_->frames > 0) {
        if (force_unread) {
            shake_->started -= std::chrono::milliseconds(
                shake_->frames * 1000 / 60);
        }
        return;
    }
    if (background_scroll_) {
        if (force_unread) {
            background_scroll_->started -= std::chrono::milliseconds(
                background_scroll_->frames * 1000 / 60);
        }
        return;
    }
    if (character_animation_active()) {
        if (force_unread) {
            for (auto& animation : character_animations_) {
                if (animation.kind != CharacterAnimationKind::none) {
                    animation.started -= std::chrono::milliseconds(
                        animation.frames * 1000 / 60);
                }
            }
        }
        return;
    }
    if (waiting_for_input_ && !force_unread && !config_.skip_unread
        && !current_text_is_read()) {
        skip_mode_ = false;
        return;
    }
    wake_time_.reset();
    if (audio_wait_) {
        waited_audio_channel().stop();
        audio_wait_.reset();
    }
    if (waiting_for_input_) {
        mark_current_text_read();
        if (finish_text_reveal()) {
            auto_next_time_.reset();
            return;
        }
        const auto reveal_start = message_.visible().size();
        if (message_.reveal_next() && message_.has_hidden_segments()) {
            start_text_reveal(reveal_start);
            auto_next_time_.reset();
            return;
        }
        waiting_for_input_ = false;
    }
    advance(true);
}


}  // namespace th2app
