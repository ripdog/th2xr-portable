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

bool Game::handle(const th2::Event& event)
{
    const auto name = event.instruction.name;
    if (name == "B" || name == "BT" || name == "BC" || name == "BCT") {
        const int scene = number(event, 1) < 0 ? -1
            : number(event, 1) * 10
                + std::max<std::int32_t>(0, number(event, 2));
        const bool unchanged_direct =
            number(event, 0) == -1
            && background_
            && bg_scene_ == scene;
        if (unchanged_direct) {
            return true;
        }
        message_visible_ = false;
        begin_transition(
            number(event, 0), number(event, 3), number(event, 6), true);
        set_background(
            event, name == "BC" || name == "BCT");
    } else if (name == "H" || name == "HT") {
        if (number(event, 1) >= 0) {
            const int visual = number(event, 1) * 10
                + std::max<std::int32_t>(0, number(event, 2));
            const bool unchanged_direct =
                number(event, 0) == -1
                && background_
                && bg_scene_ == visual;
            if (unchanged_direct) {
                return true;
            }
            message_visible_ = false;
            begin_transition(
                number(event, 0), number(event, 3), number(event, 7), true);
            set_cg(event, BackgroundKind::hcg, 'h');
        }
    } else if (name == "V" || name == "VT") {
        const int visual = number(event, 1) * 10
            + std::max<std::int32_t>(0, number(event, 2));
        const bool unchanged_direct =
            number(event, 0) == -1
            && background_
            && bg_scene_ == visual;
        if (unchanged_direct) {
            return true;
        }
        message_visible_ = false;
        begin_transition(
            number(event, 0), number(event, 3), number(event, 7), true);
        set_cg(event, BackgroundKind::visual, 'v');
    } else if (name == "FB") {
        message_visible_ = false;
        begin_background_fade(
            number(event, 0), number(event, 1), number(event, 2),
            number(event, 3));
    } else if (name == "F") {
        screen_flash_ = ScreenFlash{
            std::clamp(number(event, 0), 0, 255),
            std::clamp(number(event, 1), 0, 255),
            std::clamp(number(event, 2), 0, 255),
            std::max(1, number(event, 3)),
            std::max(1, number(event, 4)),
            std::chrono::steady_clock::now(),
        };
    } else if (name == "Q" || name == "SetShake") {
        if ((number(event, 0) == 0 || number(event, 0) == 3)
            && number(event, 2) != 0) {
            message_visible_ = false;
        }
        shake_ = ShakeState{
            number(event, 0),
            number(event, 1),
            std::max(0, number(event, 2)),
            number(event, 3),
            event.arguments.size() > 4 && number(event, 4) >= 0
                ? number(event, 4) : 256,
            0,
            std::chrono::steady_clock::now(),
        };
    } else if (name == "S") {
        begin_background_scroll(
            number(event, 0), number(event, 1), 800.0f, 600.0f,
            number(event, 2), number(event, 3));
    } else if (name == "Z") {
        begin_background_scroll(
            number(event, 0), number(event, 1),
            number(event, 2), number(event, 3),
            number(event, 4), number(event, 5) + 3);
    } else if (name == "WaitFrame") {
        const int frames = std::max<std::int32_t>(0, number(event, 0));
        wake_time_ = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(frames * 1000 / 60);
    } else if (name == "WaitTime") {
        const auto now = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto deadline = static_cast<std::uint32_t>(number(event, 0));
        const auto remaining = static_cast<std::int32_t>(deadline - now);
        if (remaining > 0) {
            wake_time_ = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(remaining);
        }
    } else if (name == "SetMovie") {
        start_movie(number(event, 0), 0, true);
    } else if (name == "SetEnding") {
        const int ending = number(event, 1) == 1 || number(event, 0) == 10
            ? 0 : number(event, 0);
        start_movie(1, ending, true);
    } else if (name == "SetTitle") {
        return_to_title();
    } else if (name == "SetDemoFlag") {
        demo_mode_ = number(event, 0) != 0;
        demo_delay_frames_ = std::max(0, number(event, 1));
        auto_next_time_.reset();
    } else if (name == "SetReplayNo") {
        const int replay = number(event, 0);
        if (unlocked_replays_.emplace(replay).second) {
            persistent_state_.unlock(
                th2::PersistentState::UnlockKind::replay, replay);
        }
    } else if (name == "ViewClock") {
        begin_clock(number(event, 0));
    } else if (name == "ViewCalender") {
        begin_calendar(number(event, 0), number(event, 1));
    } else if (name == "SkipDate") {
        skipped_month_ = number(event, 0);
        skipped_day_ = number(event, 1);
    } else if (name == "SetSakura") {
        start_sakura(number(event, 0), true);
    } else if (name == "StopSakura") {
        stop_sakura(true);
    } else if (name == "SetTimeMode") {
        const int tone = number(event, 0) < 0 ? 0 : number(event, 0);
        const int effect = number(event, 1) < 0 ? 0 : number(event, 1);
        tone_ = tone + effect * 4;
        tone_back_ = -1;
        tone_char_ = -1;
    } else if (name == "SetWeatherMode") {
        weather_ = std::max<std::int32_t>(0, number(event, 0));
    } else if (name == "SetBmpEx") {
        if (const auto slot = overlay_index(number(event, 0))) {
            load_overlay(
                *slot, text(event, 2), text(event, 6),
                number(event, 5) < 0 ? 1 : number(event, 5));
            overlay_states_[*slot].layer = number(event, 3);
        }
    } else if (name == "ResetBmp") {
        if (const auto slot = overlay_index(number(event, 0))) {
            overlays_[*slot].reset();
            overlay_pixels_[*slot].reset();
            overlay_states_[*slot] = {};
        }
    } else if (name == "SetBmpParam") {
        if (const auto slot = overlay_index(number(event, 0))) {
            overlay_states_[*slot].parameter = number(event, 1);
            overlay_states_[*slot].parameter_value =
                number(event, 2) < 0 ? 0 : number(event, 2);
        }
    } else if (name == "SetBmpBright") {
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.red = std::clamp(number(event, 1), 0, 255);
            state.green = number(event, 2) < 0
                ? state.red : std::clamp(number(event, 2), 0, 255);
            state.blue = number(event, 3) < 0
                ? state.red : std::clamp(number(event, 3), 0, 255);
            apply_overlay_brightness(*slot);
        }
    } else if (name == "SetBmpMove") {
        if (const auto slot = overlay_index(number(event, 0))) {
            overlay_states_[*slot].destination_x = number(event, 1);
            overlay_states_[*slot].destination_y = number(event, 2);
        }
    } else if (name == "SetBmpZoom") {
        if (const auto slot = overlay_index(number(event, 0))) {
            auto& state = overlay_states_[*slot];
            state.destination_x = number(event, 1);
            state.destination_y = number(event, 2);
            state.destination_width = number(event, 3);
            state.destination_height = number(event, 4);
            state.zoom = 0;
        }
    } else if (name == "C" || name == "CW") {
        set_character(event);
    } else if (name == "CR" || name == "CRW") {
        const int character_number = number(event, 0);
        const auto index = character_index(character_number);
        const int type = name == "CRW" ? 3
            : number(event, 1) < 0 ? 0 : number(event, 1);
        if (type == 3) {
            character_pending_removal_[index] = true;
        } else {
            CharacterAnimation animation;
            animation.kind = CharacterAnimationKind::leave;
            animation.type = type;
            animation.frames = number(event, 2);
            animation.blocking = true;
            if (const auto* character = characters_.find(character_number)) {
                animation.from_locate = animation.to_locate =
                    character->locate;
                animation.from_alpha = animation.to_alpha =
                    character->alpha;
                message_visible_ = false;
                start_character_animation(
                    character_number, std::move(animation));
            }
        }
    } else if (name == "CP") {
        const int character_number = number(event, 0);
        if (auto* character = characters_.find(character_number)) {
            if (character->pose == number(event, 1)) {
                return true;
            }
            if (number(event, 2) == 3) {
                character->pose = number(event, 1);
                load_character_texture(*character);
                character_staged_[character_index(character_number)] = true;
                return true;
            }
            message_visible_ = false;
            CharacterAnimation animation;
            animation.kind = CharacterAnimationKind::pose;
            animation.type = number(event, 2) < 0 ? 0 : number(event, 2);
            animation.frames = -1;
            animation.from_alpha = animation.to_alpha = character->alpha;
            animation.blocking = name == "CP";
            auto& loaded = character_texture(character_number);
            animation.previous = std::move(loaded.texture);
            loaded.pose = -1;
            character->pose = number(event, 1);
            load_character_texture(*character);
            start_character_animation(
                character_number, std::move(animation));
        }
    } else if (name == "CL") {
        if (auto* character = characters_.find(number(event, 0))) {
            message_visible_ = false;
            CharacterAnimation animation;
            animation.kind = CharacterAnimationKind::locate;
            animation.frames = name == "CL" ? number(event, 2) : -1;
            animation.from_locate = character->locate;
            animation.to_locate = number(event, 1);
            animation.blocking = name == "CL";
            character->locate = number(event, 1);
            start_character_animation(
                character->number, std::move(animation));
        }
    } else if (name == "SetMessage2") {
        push_backlog();
        message_.set(th2::substitute_player_name(
            text(event, 0), player_name_,
            runtime_.flag(213) != 0));
        current_backlog_voices_.clear();
        if (pending_backlog_voice_) {
            pending_backlog_voice_->start = 0;
            pending_backlog_voice_->end = message_.visible().size();
            current_backlog_voices_.push_back(*pending_backlog_voice_);
            pending_backlog_voice_.reset();
        }
        message_visible_ = true;
        current_line_key_ = runtime_.script_name() + ':'
            + std::to_string(runtime_.vm_pc());
        message_ends_block_ = number(event, 1) == 2;
        waiting_for_input_ = true;
        start_text_reveal(0);
        auto_next_time_.reset();
    } else if (name == "AddMessage2") {
        const auto reveal_start = message_.visible().size();
        message_.append(th2::substitute_player_name(
            text(event, 0), player_name_,
            runtime_.flag(213) != 0));
        if (pending_backlog_voice_) {
            pending_backlog_voice_->start = reveal_start;
            pending_backlog_voice_->end = message_.visible().size();
            current_backlog_voices_.push_back(*pending_backlog_voice_);
            pending_backlog_voice_.reset();
        }
        message_visible_ = true;
        current_line_key_ = runtime_.script_name() + ':'
            + std::to_string(runtime_.vm_pc());
        message_ends_block_ = number(event, 1) == 2;
        waiting_for_input_ = true;
        start_text_reveal(reveal_start);
        auto_next_time_.reset();
    } else if (name == "T") {
        message_visible_ = number(event, 0) != 0;
    } else if (name == "K") {
        waiting_for_input_ = true;
        message_ends_block_ = true;
        auto_next_time_.reset();
    } else if (name == "W") {
        // Explicit no-op in the original.
    } else if (name == "M") {
        const int music = number(event, 0);
        const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
        if (music < 0) {
            bgm_.fade_to(
                0.0f, std::chrono::milliseconds(fade * 1000 / 60), true);
            bgm_track_ = -1;
        } else {
            const int loop = number(event, 2) < 0 ? 1 : number(event, 2);
            const int volume = number(event, 3) < 0 ? 255 : number(event, 3);
            play_bgm(music, loop != 0, volume);
            if (fade > 0) {
                bgm_.set_gain(0.0f);
                bgm_.fade_to(
                    bgm_gain(volume),
                    std::chrono::milliseconds(fade * 1000 / 60));
            }
        }
    } else if (name == "MS") {
        const int fade = number(event, 0) < 0 ? 0 : number(event, 0);
        bgm_.fade_to(
            0.0f, std::chrono::milliseconds(fade * 1000 / 60), true);
        bgm_track_ = -1;
    } else if (name == "MV") {
        bgm_volume_ = number(event, 0);
        const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
        bgm_.fade_to(
            bgm_gain(bgm_volume_),
            std::chrono::milliseconds(fade * 1000 / 60));
    } else if (name == "MW") {
        if (bgm_.fading()) {
            audio_wait_ = AudioWait{AudioWaitKind::bgm, 0};
        }
    } else if (name == "SE") {
        play_se(-1, number(event, 0), false,
                number(event, 1) < 0 ? 255 : number(event, 1));
    } else if (name == "SEP") {
        play_se(
            number(event, 0), number(event, 1), number(event, 3) != 0,
            number(event, 4) < 0 ? 255 : number(event, 4),
            number(event, 2) < 0 ? 0 : number(event, 2));
    } else if (name == "SES") {
        const auto channel = number(event, 0);
        if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
            const int fade = number(event, 1) < 0 ? 0 : number(event, 1);
            se_channels_[channel].fade_to(
                0.0f, std::chrono::milliseconds(fade * 1000 / 60), true);
            se_sound_[channel] = -1;
        }
    } else if (name == "SEV") {
        const auto channel = number(event, 0);
        if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
            se_volume_[channel] = number(event, 1);
            const int fade = number(event, 2) < 0 ? 0 : number(event, 2);
            se_channels_[channel].fade_to(
                se_gain(se_volume_[channel]),
                std::chrono::milliseconds(fade * 1000 / 60));
        }
    } else if (name == "SEW") {
        const auto channel = number(event, 0);
        const bool wait_for_playback = channel >= 0
            && static_cast<std::size_t>(channel) < se_channels_.size()
            && !se_loop_[channel] && se_channels_[channel].playing();
        if (wait_for_playback) {
            audio_wait_ = AudioWait{
                AudioWaitKind::sound_effect, static_cast<std::size_t>(channel)};
        }
    } else if (name == "VV" || name == "VA" || name == "VB"
               || name == "VC") {
        play_voice(event);
    } else if (name == "VI") {
        if (number(event, 2) != -1) {
            vi_event_voice_no_all_ = number(event, 2);
        } else if (number(event, 1) != -1) {
            vi_event_voice_no_ = number(event, 1);
        }
    } else if (name == "VS") {
        const auto channel = number(event, 1) < 0 ? 0 : number(event, 1);
        if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()) {
            const int fade = number(event, 0) < 0 ? 0 : number(event, 0);
            voice_channels_[channel].fade_to(
                0.0f, std::chrono::milliseconds(fade * 1000 / 60), true);
            voice_sound_[channel] = -1;
        }
    } else if (name == "VW") {
        const auto channel = number(event, 0) < 0 ? 0 : number(event, 0);
        if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()
            && voice_channels_[channel].playing()) {
            audio_wait_ = AudioWait{
                AudioWaitKind::voice, static_cast<std::size_t>(channel)};
        }
    } else if (name == "SetSelectMes") {
        choices_.push_back(Choice{
            interpret_newlines(th2::substitute_player_name(
                text(event, 0), player_name_,
                runtime_.flag(213) != 0)),
            number(event, 1),
            number(event, 2),
        });
    } else if (name == "SetSelect") {
        choice_result_register_ =
            std::get<th2::RegisterTarget>(event.arguments.at(0)).index;
        choosing_ = true;
        choice_highlight_ = 0;
        choice_selected_ = -1;
    } else if (name == "SetMapEvent") {
        map_events_.push_back(MapEvent{
            number(event, 0), number(event, 1), number(event, 2),
            text(event, 3)});
    } else if (name == "LoadScript") {
        for (std::size_t i = 0; i < overlays_.size(); ++i) {
            overlays_[i].reset();
            overlay_pixels_[i].reset();
            overlay_states_[i] = {};
        }
        choices_.clear();
        choosing_ = false;
        choice_highlight_ = 0;
        choice_selected_ = -1;
        choice_result_register_ = -1;
        choice_ex_ = false;
        waiting_for_input_ = false;
        message_ends_block_ = false;
        load_script(text(event, 0));
    } else if (name == "SetSelectMes") {
        choices_.push_back(Choice{
            interpret_newlines(th2::substitute_player_name(
                text(event, 0), player_name_,
                runtime_.flag(213) != 0)),
            number(event, 1),
            number(event, 2),
        });
    } else if (name == "SetSelect") {
        choice_result_register_ =
            std::get<th2::RegisterTarget>(event.arguments.at(0)).index;
        choosing_ = true;
        choice_highlight_ = 0;
        choice_selected_ = -1;
    } else {
        return false;
    }
    return true;
}

std::filesystem::path Game::dump_engine_error(
    const th2::ScriptStep& step, std::string_view error)
{
    const auto logs_dir = writable_directory() / "logs";
    std::filesystem::create_directories(logs_dir);
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const auto path = logs_dir / std::format("engine-error-{}.log", stamp);
    std::ofstream output(path);
    output << "error=" << error << '\n'
           << "script=" << step.script_name << '\n'
           << "pc=" << step.event.instruction.offset << '\n'
           << "next_pc=" << runtime_.vm_pc() << '\n'
           << "opcode=" << step.event.instruction.opcode << '\n'
           << "instruction=" << step.event.instruction.name << '\n'
           << "arguments=";
    for (std::size_t i = 0; i < step.event.arguments.size(); ++i) {
        if (i != 0) {
            output << ", ";
        }
        std::visit([&](const auto& argument) {
            using T = std::decay_t<decltype(argument)>;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                output << argument;
            } else if constexpr (std::is_same_v<T, std::string>) {
                output << std::quoted(argument);
            } else if constexpr (std::is_same_v<T, th2::RegisterTarget>) {
                output << "register[" << static_cast<int>(argument.index) << ']';
            } else {
                output << "compare(register["
                       << static_cast<int>(argument.register_index)
                       << "], op=" << static_cast<int>(argument.operation)
                       << ", value=" << argument.value << ')';
            }
        }, step.event.arguments[i]);
    }
    output << "\nregisters=";
    for (const auto value : runtime_.vm_registers()) {
        output << value << ' ';
    }
    output << "\nstack=";
    for (const auto value : runtime_.vm_stack()) {
        output << value << ' ';
    }
    output << "\nbackground=" << bg_scene_
           << "\nbackground_kind="
           << static_cast<std::int32_t>(background_kind_)
           << "\ntone=" << tone_
           << "\ntone_back=" << tone_back_
           << "\ntone_char=" << tone_char_
           << "\nweather=" << weather_
           << "\nbgm=" << bgm_track_
           << "\nvoice_event=" << vi_event_voice_no_
           << "\nvoice_event_all=" << vi_event_voice_no_all_
           << "\nmessage=" << std::quoted(message_.visible()) << '\n';
    return path;
}

std::filesystem::path Game::dump_runtime_error(std::string_view error)
{
    const auto logs_dir = writable_directory() / "logs";
    std::filesystem::create_directories(logs_dir);
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto path = logs_dir / std::format("engine-error-{}.log", stamp);
    std::ofstream output(path);
    const auto pc = runtime_.vm_pc();
    const auto bytecode = runtime_.vm_bytecode();
    output << "error=" << error << '\n'
           << "script=" << runtime_.script_name() << '\n'
           << "pc=" << pc << "\nbytecode=";
    const auto first = pc > 16 ? pc - 16 : 0;
    const auto last = std::min(bytecode.size(), pc + 32);
    output << std::hex << std::setfill('0');
    for (std::size_t offset = first; offset < last; ++offset) {
        if (offset == pc) {
            output << '[';
        }
        output << std::setw(2)
               << static_cast<unsigned>(bytecode[offset]);
        if (offset == pc + 1) {
            output << ']';
        }
        output << ' ';
    }
    output << std::dec << "\nregisters=";
    for (const auto value : runtime_.vm_registers()) {
        output << value << ' ';
    }
    output << "\nstack=";
    for (const auto value : runtime_.vm_stack()) {
        output << value << ' ';
    }
    output << "\nbackground=" << bg_scene_
           << "\nbackground_kind="
           << static_cast<std::int32_t>(background_kind_)
           << "\ntone=" << tone_
           << "\ntone_back=" << tone_back_
           << "\ntone_char=" << tone_char_
           << "\nweather=" << weather_
           << "\nbgm=" << bgm_track_
           << "\nvoice_event=" << vi_event_voice_no_
           << "\nvoice_event_all=" << vi_event_voice_no_all_
           << "\nmessage=" << std::quoted(message_.visible()) << '\n';
    return path;
}

void Game::advance(bool skipping)
{
    if (wake_time_ || audio_wait_ || transition_ || background_fade_
        || screen_flash_
        || (shake_ && shake_->frames > 0)
        || background_scroll_ || character_animation_active()
        || clock_state_ || calendar_state_
        || movie_) {
        return;
    }
    // Record whether the player is advancing past a block end (page end).
    // Used at the end of advance() to decide whether to autosave.
    just_advanced_past_block_end_ = false;
    if (waiting_for_input_ && message_ends_block_ && !message_.has_hidden_segments()) {
        just_advanced_past_block_end_ = true;
    }
    if (waiting_for_input_) {
        mark_current_text_read();
    }
    if (waiting_for_input_ && finish_text_reveal()) {
        auto_next_time_.reset();
        return;
    }
    const auto reveal_start = message_.visible().size();
    if (waiting_for_input_ && message_.reveal_next()) {
        start_text_reveal(reveal_start);
        auto_next_time_.reset();
        return;
    }
    waiting_for_input_ = false;
    if (choosing_) {
        if (choice_selected_ < 0) {
            return;
        }
        if (choice_ex_) {
            load_script(choices_.at(choice_selected_).sno);
        } else if (choice_result_register_ >= 0) {
            runtime_.set_reg(
                static_cast<std::size_t>(choice_result_register_),
                choice_selected_);
        }
        choices_.clear();
        choosing_ = false;
        choice_highlight_ = 0;
        choice_selected_ = -1;
        choice_result_register_ = -1;
        choice_ex_ = false;
    }
    while (running_ && !waiting_for_input_ && !choosing_) {
        th2::ScriptStep step;
        try {
            step = runtime_.run();
        } catch (const std::exception& error) {
            const auto dump = dump_runtime_error(error.what());
            throw std::runtime_error(std::format(
                "{}:{}: {} (state dumped to {})",
                runtime_.script_name(), runtime_.vm_pc(),
                error.what(), dump.string()));
        }
        sync_game_flags();
        if (step.reason == th2::VmYield::ended) {
            if (replay_mode_ || direct_scenario_) {
                return_to_title();
                break;
            }
            if (!load_scheduled_script()) {
                return_to_title();
                break;
            }
            if (ui_mode_ == UiMode::map || calendar_state_) {
                break;
            }
            continue;
        }
        if (step.reason == th2::VmYield::wait_frames
            || step.reason == th2::VmYield::wait_time) {
            if (skipping) {
                continue;
            }
            const auto milliseconds = step.reason == th2::VmYield::wait_frames
                ? step.wait_value * 1000 / 60
                : step.wait_value;
            wake_time_ = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(milliseconds);
            break;
        }
        if (step.reason == th2::VmYield::frame) {
            wake_time_ = std::chrono::steady_clock::now();
            break;
        }
        if (step.reason == th2::VmYield::event) {
            try {
                if (!handle(step.event)) {
                    throw std::runtime_error(std::format(
                        "unimplemented event opcode: {}",
                        step.event.instruction.name));
                }
            } catch (const std::exception& error) {
                const auto dump = dump_engine_error(step, error.what());
                throw std::runtime_error(std::format(
                    "{}:{}: {} (state dumped to {})",
                    step.script_name, step.event.instruction.offset,
                    error.what(), dump.string()));
            }
            if (audio_wait_) {
                if (skipping) {
                    if (audio_wait_->kind == AudioWaitKind::bgm) {
                        waited_audio_channel().finish_fade();
                    } else {
                        waited_audio_channel().stop();
                    }
                    audio_wait_.reset();
                    continue;
                }
                break;
            }
            if (wake_time_) {
                break;
            }
            if (transition_) {
                break;
            }
            if (background_fade_) {
                break;
            }
            if (screen_flash_) {
                break;
            }
            if (shake_ && shake_->frames > 0) {
                break;
            }
            if (background_scroll_) {
                break;
            }
            if (character_animation_active()) {
                break;
            }
            if (clock_state_ || calendar_state_) {
                break;
            }
            if (ui_mode_ != UiMode::game) {
                break;
            }
            if (skipping && waiting_for_input_) {
                break;
            }
            if (choosing_) {
                break;
            }
        }
    }
    // Autosave after advancing past a block end, if enabled and enough time has passed.
    if (just_advanced_past_block_end_) {
        just_advanced_past_block_end_ = false;
        if (config_.autosave_enabled && ui_mode_ == UiMode::game
            && !replay_mode_ && !demo_mode_) {
            const auto now = std::chrono::steady_clock::now();
            constexpr auto minimum_interval = std::chrono::minutes(2);
            if (last_save_time_.time_since_epoch().count() == 0
                || now - last_save_time_ >= minimum_interval) {
                if (!save_snapshot_) {
                    save_snapshot_ = capture_frame_pixels();
                }
                perform_autosave();
                // Refresh the save snapshot so the next autosave has an
                // up-to-date thumbnail.
                save_snapshot_.reset();
            }
        }
    }
}


}  // namespace th2app
