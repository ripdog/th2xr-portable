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

void Game::save(int slot)
{
    if (replay_mode_ || wake_time_ || audio_wait_ || transition_
        || background_fade_ || screen_flash_
        || (shake_ && shake_->frames > 0)
        || background_scroll_ || character_animation_active()
        || clock_state_ || calendar_state_ || movie_) {
        return;
    }
    const auto save_dir = writable_directory() / "save";
    std::filesystem::create_directories(save_dir);
    const auto path = save_path(slot);
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return;
    }
    save_body(file);
    file.close();
    save_preview(slot);
    last_save_time_ = std::chrono::steady_clock::now();
}

bool Game::load(int slot)
{
    const auto path = save_path(slot);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    return load_body(file);
}

std::filesystem::path Game::save_path(int slot) const
{
    return writable_directory() / "save"
        / std::format("save_{:02d}.sav", slot);
}

std::filesystem::path Game::thumbnail_path(int slot) const
{
    return writable_directory() / "save"
        / std::format("save_{:02d}.bmp", slot);
}

std::filesystem::path Game::metadata_path(int slot) const
{
    return writable_directory() / "save"
        / std::format("save_{:02d}.meta", slot);
}

void Game::save_preview(int slot)
{
    if (save_snapshot_) {
        Surface thumbnail(SDL_CreateSurface(160, 120, SDL_PIXELFORMAT_RGBA32));
        if (thumbnail
            && SDL_BlitSurfaceScaled(
                save_snapshot_.get(), nullptr, thumbnail.get(), nullptr,
                SDL_SCALEMODE_LINEAR)) {
            SDL_SaveBMP(thumbnail.get(), thumbnail_path(slot).string().c_str());
        }
    }
    std::ofstream metadata(metadata_path(slot));
    if (metadata) {
        auto excerpt = message_.visible();
        std::replace(excerpt.begin(), excerpt.end(), '\n', ' ');
        metadata << std::time(nullptr) << '\n'
                 << runtime_.flag(0) << ' ' << runtime_.flag(1) << ' '
                 << runtime_.flag(2) << '\n'
                 << excerpt.substr(0, 18) << '\n';
    }
}

Game::SaveMetadata Game::read_save_metadata(int slot) const
{
    SaveMetadata result;
    result.exists = std::filesystem::exists(save_path(slot));
    if (!result.exists) {
        return result;
    }
    std::ifstream metadata(metadata_path(slot));
    if (metadata) {
        long long timestamp = 0;
        metadata >> timestamp;
        metadata.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        auto after_timestamp = metadata.tellg();
        if (metadata >> result.game_month >> result.game_day
                     >> result.game_time) {
            metadata.ignore(
                std::numeric_limits<std::streamsize>::max(), '\n');
        } else {
            metadata.clear();
            metadata.seekg(after_timestamp);
        }
        std::getline(metadata, result.message);
        result.timestamp = static_cast<std::time_t>(timestamp);
    }
    if (result.timestamp == 0) {
        const auto written = std::filesystem::last_write_time(save_path(slot));
        const auto sys_time = std::chrono::file_clock::to_sys(written);
        result.timestamp = std::chrono::system_clock::to_time_t(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(sys_time));
    }
    return result;
}

void Game::perform_autosave()
{
    // Autosave slots are 100-109, reusing the same save/load infrastructure.
    // Priority: lowest-numbered empty slot, then oldest occupied slot.
    constexpr int autosave_base = 100;
    int target_slot = -1;
    std::time_t oldest_time = std::numeric_limits<std::time_t>::max();
    for (int i = 0; i < 10; ++i) {
        const auto metadata = read_save_metadata(autosave_base + i);
        if (!metadata.exists) {
            target_slot = autosave_base + i;
            break;
        }
        if (metadata.timestamp < oldest_time) {
            oldest_time = metadata.timestamp;
            target_slot = autosave_base + i;
        }
    }
    if (target_slot < 0) {
        return;
    }
    save(target_slot);
}

void Game::refresh_save_page()
{
    constexpr int autosave_base = 100;
    // Page 10 (displayed as 11/11) shows autosave slots 100-109.
    if (save_page_ == 10) {
        newest_save_slot_ = -1;
        std::time_t newest_time = 0;
        for (int slot = 0; slot < 100; ++slot) {
            const auto metadata = read_save_metadata(slot);
            if (metadata.exists && metadata.timestamp >= newest_time) {
                newest_time = metadata.timestamp;
                newest_save_slot_ = slot;
            }
        }
        for (int i = 0; i < 10; ++i) {
            const auto metadata = read_save_metadata(autosave_base + i);
            if (metadata.exists && metadata.timestamp >= newest_time) {
                newest_time = metadata.timestamp;
                newest_save_slot_ = autosave_base + i;
            }
        }
        for (int i = 0; i < 10; ++i) {
            const int slot = autosave_base + i;
            visible_saves_[i] = read_save_metadata(slot);
            save_thumbnails_[i].reset();
            if (!visible_saves_[i].exists) {
                continue;
            }
            SDL_Surface* surface =
                SDL_LoadBMP(thumbnail_path(slot).string().c_str());
            if (surface) {
                save_thumbnails_[i] = texture_from_surface(surface);
                SDL_DestroySurface(surface);
            }
        }
        return;
    }
    newest_save_slot_ = -1;
    std::time_t newest_time = 0;
    for (int slot = 0; slot < 100; ++slot) {
        const auto metadata = read_save_metadata(slot);
        if (metadata.exists && metadata.timestamp >= newest_time) {
            newest_time = metadata.timestamp;
            newest_save_slot_ = slot;
        }
    }
    for (int i = 0; i < 10; ++i) {
        const auto metadata = read_save_metadata(autosave_base + i);
        if (metadata.exists && metadata.timestamp >= newest_time) {
            newest_time = metadata.timestamp;
            newest_save_slot_ = autosave_base + i;
        }
    }
    for (int i = 0; i < 10; ++i) {
        const int slot = save_page_ * 10 + i;
        visible_saves_[i] = read_save_metadata(slot);
        save_thumbnails_[i].reset();
        if (!visible_saves_[i].exists) {
            continue;
        }
        SDL_Surface* surface =
            SDL_LoadBMP(thumbnail_path(slot).string().c_str());
        if (surface) {
            save_thumbnails_[i] = texture_from_surface(surface);
            SDL_DestroySurface(surface);
        }
    }
}

void Game::save_body(std::ostream& out) const
{
    write_u32(out, save_version_);  // native version

    // Script identity
    write_str(out, runtime_.script_name(), 64);
    write_i32(out, tone_);
    write_i32(out, tone_back_);
    write_i32(out, tone_char_);
    write_i32(out, weather_);
    write_i32(out, vi_event_voice_no_);
    write_i32(out, vi_event_voice_no_all_);
    write_i32(out, demo_mode_ ? 1 : 0);
    write_i32(out, demo_delay_frames_);
    write_i32(out, skipped_month_);
    write_i32(out, skipped_day_);
    write_i32(out, shake_.has_value() ? 1 : 0);
    if (shake_) {
        write_i32(out, shake_->type);
        write_i32(out, shake_->pitch);
        write_i32(out, shake_->frames);
        write_i32(out, shake_->direction);
        write_i32(out, shake_->swing);
        write_i32(out, shake_->sampled_frame);
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - shake_->started).count();
        write_i64(out, elapsed);
    }
    write_i32(out, sakura_.has_value() ? 1 : 0);
    write_u32(out, sakura_random_);
    if (sakura_) {
        write_i32(out, sakura_->amount);
        write_i32(out, sakura_->target_amount);
        write_i32(out, static_cast<std::int32_t>(sakura_->wind * 1000.0f));
        write_i32(out, sakura_->speed);
        write_i32(out, sakura_->tick);
        write_i32(out, sakura_->reset_frames);
        write_i32(out, sakura_->no_reset ? 1 : 0);
        for (int i = 0; i < sakura_->amount; ++i) {
            const auto& petal = sakura_->petals[i];
            write_i32(out, petal.active ? 1 : 0);
            write_i32(out, petal.type);
            write_i32(out, static_cast<std::int32_t>(petal.x * 1000.0f));
            write_i32(out, static_cast<std::int32_t>(petal.y * 1000.0f));
            write_i32(
                out, static_cast<std::int32_t>(petal.axis_x * 1000.0f));
            write_i32(
                out, static_cast<std::int32_t>(petal.axis_y * 1000.0f));
            write_u32(out, petal.counter);
        }
    }

    // VM state
    write_u32(out, static_cast<std::uint32_t>(runtime_.vm_pc()));
    const auto registers = runtime_.vm_registers();
    for (const auto r : registers) {
        write_i32(out, r);
    }
    const auto stack = runtime_.vm_stack();
    write_u32(out, static_cast<std::uint32_t>(stack.size()));
    for (const auto s : stack) {
        write_i32(out, s);
    }

    // Flags
    for (const auto f : runtime_.all_flags()) {
        write_i32(out, f);
    }

    // Background
    write_i32(out, bg_scene_);
    write_i32(out, static_cast<std::int32_t>(background_kind_));
    write_i32(out, static_cast<std::int32_t>(background_view_.x));
    write_i32(out, static_cast<std::int32_t>(background_view_.y));
    write_i32(out, static_cast<std::int32_t>(background_view_.width));
    write_i32(out, static_cast<std::int32_t>(background_view_.height));
    for (const float brightness : background_brightness_) {
        write_i32(out, static_cast<std::int32_t>(brightness));
    }

    // Characters
    const auto ordered = characters_.ordered();
    write_u32(out, static_cast<std::uint32_t>(ordered.size()));
    for (const auto& ch : ordered) {
        write_i32(out, ch.number);
        write_i32(out, ch.pose);
        write_i32(out, ch.locate);
        write_i32(out, ch.layer);
        write_i32(out, ch.brightness);
        write_i32(out, ch.alpha);
        write_i32(out, character_staged_.at(ch.number) ? 1 : 0);
        write_i32(
            out, character_pending_removal_.at(ch.number) ? 1 : 0);
    }

    // Overlays
    std::uint32_t overlay_count = 0;
    for (const auto& ov : overlays_) {
        if (ov) {
            ++overlay_count;
        }
    }
    write_u32(out, overlay_count);
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (!overlays_[i]) {
            continue;
        }
        const auto& state = overlay_states_[i];
        write_u32(out, static_cast<std::uint32_t>(i));
        write_str(out, state.name, 64);
        write_str(out, state.archive, 16);
        write_i32(out, state.visible ? 1 : 0);
        write_i32(out, state.layer);
        write_i32(out, state.tone_type);
        write_i32(out, state.parameter);
        write_i32(out, state.parameter_value);
        write_i32(out, state.reverse);
        write_i32(out, state.red);
        write_i32(out, state.green);
        write_i32(out, state.blue);
        write_i32(out, state.destination_x);
        write_i32(out, state.destination_y);
        write_i32(out, state.destination_width);
        write_i32(out, state.destination_height);
        write_i32(out, state.source_x);
        write_i32(out, state.source_y);
        write_i32(out, state.source_width);
        write_i32(out, state.source_height);
        write_i32(out, state.zoom_center_x);
        write_i32(out, state.zoom_center_y);
        write_i32(out, state.zoom);
    }

    // BGM
    write_i32(out, bgm_track_);
    write_i32(out, bgm_loop_ ? 1 : 0);
    write_i32(out, bgm_volume_);

    // The original restores only looping numbered SE channels.
    std::uint32_t se_ch_count = 0;
    for (std::size_t i = 0; i < se_channels_.size(); ++i) {
        if (se_channels_[i].playing() && se_loop_[i]) {
            ++se_ch_count;
        }
    }
    write_u32(out, se_ch_count);
    for (std::size_t i = 0; i < se_channels_.size(); ++i) {
        if (se_channels_[i].playing() && se_loop_[i]) {
            write_u32(out, static_cast<std::uint32_t>(i));
            write_i32(out, se_sound_[i]);
            write_i32(out, se_volume_[i]);
        }
    }

    // Message state - full segments for correct text-block position
    write_i32(out, message_.empty() ? 0 : 1);
    const auto& segments = message_.segments();
    write_u32(out, static_cast<std::uint32_t>(segments.size()));
    for (const auto& seg : segments) {
        write_u32(out, static_cast<std::uint32_t>(seg.size()));
        if (!seg.empty()) {
            out.write(seg.data(), static_cast<std::streamsize>(seg.size()));
        }
    }
    write_u32(out, static_cast<std::uint32_t>(message_.revealed_count()));
    const auto& visible = message_.visible();
    write_u32(out, static_cast<std::uint32_t>(visible.size()));
    if (!visible.empty()) {
        out.write(visible.data(), static_cast<std::streamsize>(visible.size()));
    }

    // Choice state
    write_i32(out, choosing_ ? 1 : 0);
    write_u32(out, static_cast<std::uint32_t>(choices_.size()));
    for (const auto& c : choices_) {
        write_str(out, c.text, 256);
        write_i32(out, c.flag_no);
        write_i32(out, c.flag_value);
        write_str(out, c.sno, 8);
    }
    write_i32(out, choice_highlight_);
    write_i32(out, choice_selected_);
    write_i32(out, choice_result_register_);
    write_i32(out, choice_ex_ ? 1 : 0);

    // Pending after-school destinations survive saves made in a mandatory
    // scene that runs before the map opens.
    write_u32(out, static_cast<std::uint32_t>(map_events_.size()));
    for (const auto& event : map_events_) {
        write_i32(out, event.character);
        write_i32(out, event.position);
        write_i32(out, event.type);
        write_str(out, event.script, 32);
    }

    // Backlog state. Depth 0 is the current message; 1 is newest history.
    write_u32(out, static_cast<std::uint32_t>(backlog_.size()));
    for (const auto& entry : backlog_) {
        write_u32(out, static_cast<std::uint32_t>(entry.text.size()));
        out.write(entry.text.data(),
                  static_cast<std::streamsize>(entry.text.size()));
        write_u32(out, static_cast<std::uint32_t>(entry.voices.size()));
        for (const auto& voice : entry.voices) {
            write_u32(out, static_cast<std::uint32_t>(voice.start));
            write_u32(out, static_cast<std::uint32_t>(voice.end));
            write_i32(out, voice.scenario);
            write_i32(out, voice.voice);
            write_i32(out, voice.character);
            write_i32(out, voice.volume);
            write_i32(out, voice.alternate ? 1 : 0);
        }
    }
    write_i32(out, backlog_depth_);
    write_i32(out, ui_mode_ == UiMode::backlog ? 1 : 0);
    write_i32(out, message_ends_block_ ? 1 : 0);

    // Playback and read state
    write_u32(out, static_cast<std::uint32_t>(current_line_key_.size()));
    out.write(current_line_key_.data(),
              static_cast<std::streamsize>(current_line_key_.size()));
    write_i32(out, auto_mode_ ? 1 : 0);
    write_i32(out, skip_mode_ ? 1 : 0);
    write_u32(
        out, static_cast<std::uint32_t>(config_.read_lines.size()));
    for (const auto& key : config_.read_lines) {
        write_u32(out, static_cast<std::uint32_t>(key.size()));
        out.write(key.data(), static_cast<std::streamsize>(key.size()));
    }
    const std::array saved_names{
        &config_.player_name.family,
        &config_.player_name.given,
        &config_.player_name.family_reading,
        &config_.player_name.given_reading,
        &config_.player_name.nickname,
        &config_.player_name.nickname_reading,
    };
    for (const auto* value : saved_names) {
        write_u32(out, static_cast<std::uint32_t>(value->size()));
        out.write(value->data(), static_cast<std::streamsize>(value->size()));
    }
}

bool Game::load_body(std::istream& in)
{
    const auto version = read_u32(in);
    if (version < oldest_supported_save_version_
        || version > save_version_) {
        return false;
    }

    reset_play_state();
    ui_mode_ = UiMode::game;
    message_visible_ = true;

    // Script identity
    const auto script_name = read_str(in, 64);
    runtime_.load(script_name);
    tone_ = read_i32(in);
    tone_back_ = read_i32(in);
    tone_char_ = read_i32(in);
    weather_ = read_i32(in);
    vi_event_voice_no_ = read_i32(in);
    vi_event_voice_no_all_ = read_i32(in);
    demo_mode_ = read_i32(in) != 0;
    demo_delay_frames_ = read_i32(in);
    skipped_month_ = read_i32(in);
    skipped_day_ = read_i32(in);
    if (read_i32(in)) {
        ShakeState state;
        state.type = read_i32(in);
        state.pitch = read_i32(in);
        state.frames = read_i32(in);
        state.direction = read_i32(in);
        state.swing = read_i32(in);
        state.sampled_frame = read_i32(in);
        state.started = std::chrono::steady_clock::now()
            - std::chrono::milliseconds(read_i64(in));
        shake_ = state;
    } else {
        shake_.reset();
    }
    const bool has_sakura = read_i32(in) != 0;
    sakura_random_ = read_u32(in);
    if (has_sakura) {
        SakuraState state;
        state.amount = read_i32(in);
        state.target_amount = read_i32(in);
        state.wind = read_i32(in) / 1000.0f;
        state.speed = read_i32(in);
        state.tick = read_i32(in);
        state.reset_frames = read_i32(in);
        state.no_reset = read_i32(in) != 0;
        state.updated = std::chrono::steady_clock::now();
        if (state.amount < 0
            || state.amount > static_cast<int>(state.petals.size())) {
            throw std::runtime_error("invalid sakura save state");
        }
        for (int i = 0; i < state.amount; ++i) {
            auto& petal = state.petals[i];
            petal.active = read_i32(in) != 0;
            petal.type = read_i32(in);
            petal.x = read_i32(in) / 1000.0f;
            petal.y = read_i32(in) / 1000.0f;
            petal.axis_x = read_i32(in) / 1000.0f;
            petal.axis_y = read_i32(in) / 1000.0f;
            petal.counter = read_u32(in);
        }
        sakura_large_ = load_sakura_texture("sakura.bmp");
        sakura_small_ = load_sakura_texture("sakura2.bmp");
        sakura_ = std::move(state);
    } else {
        sakura_.reset();
    }

    // VM state
    const auto pc = read_u32(in);
    std::array<std::int32_t, 50> regs{};
    for (auto& r : regs) {
        r = read_i32(in);
    }
    const auto stack_size = read_u32(in);
    std::vector<std::int32_t> stack_data;
    stack_data.reserve(stack_size);
    for (std::uint32_t i = 0; i < stack_size; ++i) {
        stack_data.push_back(read_i32(in));
    }
    // Flags
    for (std::size_t i = 0; i < 1024; ++i) {
        runtime_.set_flag(i, read_i32(in));
    }
    runtime_.vm_restore(regs, stack_data, pc);

    // Background
    bg_scene_ = read_i32(in);
    background_kind_ =
        static_cast<BackgroundKind>(read_i32(in));
    background_view_.x = static_cast<float>(read_i32(in));
    background_view_.y = static_cast<float>(read_i32(in));
    background_view_.width = static_cast<float>(read_i32(in));
    background_view_.height = static_cast<float>(read_i32(in));
    for (auto& brightness : background_brightness_) {
        brightness = static_cast<float>(read_i32(in));
    }
    background_scroll_.reset();
    restore_background();

    // Characters
    characters_ = {};
    character_textures_ = {};
    character_animations_ = {};
    character_staged_ = {};
    character_pending_removal_ = {};
    const auto char_count = read_u32(in);
    for (std::uint32_t i = 0; i < char_count; ++i) {
        const auto number = read_i32(in);
        const auto pose = read_i32(in);
        const auto locate = read_i32(in);
        const auto layer = read_i32(in);
        const auto brightness = read_i32(in);
        const auto alpha = read_i32(in);
        const bool staged = read_i32(in) != 0;
        const bool pending_removal = read_i32(in) != 0;
        auto& ch = characters_.set(
            number, pose, locate, layer, brightness, alpha);
        character_staged_.at(number) = staged;
        character_pending_removal_.at(number) = pending_removal;
        load_character_texture(ch);
    }

    // Overlays
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        overlays_[i].reset();
        overlay_pixels_[i].reset();
        overlay_states_[i] = {};
    }
    const auto overlay_count = read_u32(in);
    for (std::uint32_t i = 0; i < overlay_count; ++i) {
        const auto slot = static_cast<std::size_t>(read_u32(in));
        const auto name = read_str(in, 64);
        const auto archive = read_str(in, 16);
        OverlayState state;
        state.name = name;
        state.archive = archive;
        state.visible = read_i32(in) != 0;
        state.layer = read_i32(in);
        state.tone_type = read_i32(in);
        state.parameter = read_i32(in);
        state.parameter_value = read_i32(in);
        state.reverse = read_i32(in);
        state.red = read_i32(in);
        state.green = read_i32(in);
        state.blue = read_i32(in);
        state.destination_x = read_i32(in);
        state.destination_y = read_i32(in);
        state.destination_width = read_i32(in);
        state.destination_height = read_i32(in);
        state.source_x = read_i32(in);
        state.source_y = read_i32(in);
        state.source_width = read_i32(in);
        state.source_height = read_i32(in);
        state.zoom_center_x = read_i32(in);
        state.zoom_center_y = read_i32(in);
        state.zoom = read_i32(in);
        if (slot < overlays_.size()) {
            load_overlay(slot, name, archive, state.tone_type);
            overlay_states_[slot] = std::move(state);
            apply_overlay_brightness(slot);
        }
    }

    // BGM
    const auto loaded_bgm_track = read_i32(in);
    bgm_loop_ = read_i32(in) != 0;
    bgm_volume_ = read_i32(in);
    if (loaded_bgm_track >= 0) {
        play_bgm(loaded_bgm_track, bgm_loop_, bgm_volume_);
    }

    // SE channels - restore looping channels
    const auto se_ch_count = read_u32(in);
    for (std::uint32_t i = 0; i < se_ch_count; ++i) {
        const auto channel = static_cast<std::size_t>(read_u32(in));
        const auto sound = read_i32(in);
        const auto volume = read_i32(in);
        if (channel < se_channels_.size() && sound >= 0) {
            play_se(static_cast<int>(channel), sound, true, volume);
        }
    }

    // Message state - restore full segments
    const auto has_message = read_i32(in);
    if (has_message) {
        const auto seg_count = read_u32(in);
        std::vector<std::string> segments;
        segments.reserve(seg_count);
        for (std::uint32_t i = 0; i < seg_count; ++i) {
            const auto seg_size = read_u32(in);
            std::string seg(seg_size, '\0');
            if (seg_size > 0) {
                in.read(seg.data(),
                        static_cast<std::streamsize>(seg_size));
            }
            segments.push_back(std::move(seg));
        }
        const auto revealed = read_u32(in);
        const auto visible_size = read_u32(in);
        std::string visible(visible_size, '\0');
        if (visible_size > 0) {
            in.read(visible.data(),
                    static_cast<std::streamsize>(visible_size));
        }
        message_.restore_state(segments, revealed, visible);
        waiting_for_input_ = true;
    } else {
        message_ = th2::Message{};
        waiting_for_input_ = false;
    }

    // Choice state
    choosing_ = read_i32(in) != 0;
    choices_.clear();
    const auto choices_count = read_u32(in);
    for (std::uint32_t i = 0; i < choices_count; ++i) {
        choices_.push_back(Choice{
            read_str(in, 256),
            read_i32(in),
            read_i32(in),
            read_str(in, 8),
        });
    }
    choice_highlight_ = read_i32(in);
    choice_selected_ = read_i32(in);
    choice_result_register_ = read_i32(in);
    choice_ex_ = read_i32(in) != 0;

    map_events_.clear();
    const auto map_event_count = read_u32(in);
    map_events_.reserve(map_event_count);
    for (std::uint32_t i = 0; i < map_event_count; ++i) {
        map_events_.push_back(MapEvent{
            read_i32(in), read_i32(in), read_i32(in),
            read_str(in, 32),
        });
    }

    backlog_.clear();
    current_backlog_voices_.clear();
    pending_backlog_voice_.reset();
    backlog_depth_ = 0;
    const auto backlog_count = read_u32(in);
    backlog_.reserve(backlog_count);
    for (std::uint32_t i = 0; i < backlog_count; ++i) {
        const auto size = read_u32(in);
        std::string history_text(size, '\0');
        if (size > 0) {
            in.read(history_text.data(),
                    static_cast<std::streamsize>(size));
        }
        std::vector<BacklogVoice> voices;
        if (version >= first_backlog_voice_save_version_) {
            const auto voice_count = read_u32(in);
            voices.reserve(voice_count);
            for (std::uint32_t v = 0; v < voice_count; ++v) {
                voices.push_back(BacklogVoice{
                    read_u32(in),
                    read_u32(in),
                    read_i32(in),
                    read_i32(in),
                    read_i32(in),
                    read_i32(in),
                    read_i32(in) != 0,
                });
            }
        }
        backlog_.push_back({std::move(history_text), std::move(voices)});
    }
    backlog_depth_ = std::clamp(
        read_i32(in), 0, static_cast<int>(backlog_.size()));
    if (read_i32(in) != 0) {
        ui_mode_ = UiMode::backlog;
    }
    message_ends_block_ = read_i32(in) != 0;
    const auto key_size = read_u32(in);
    current_line_key_.resize(key_size);
    in.read(current_line_key_.data(),
            static_cast<std::streamsize>(key_size));
    auto_mode_ = read_i32(in) != 0;
    skip_mode_ = read_i32(in) != 0;
    const auto read_count = read_u32(in);
    for (std::uint32_t i = 0; i < read_count; ++i) {
        const auto size = read_u32(in);
        std::string key(size, '\0');
        in.read(key.data(), static_cast<std::streamsize>(size));
        config_.read_lines.insert(std::move(key));
    }
    const auto read_name = [&]() {
        const auto size = read_u32(in);
        std::string value(size, '\0');
        in.read(value.data(), static_cast<std::streamsize>(size));
        return value;
    };
    config_.player_name.family = read_name();
    config_.player_name.given = read_name();
    config_.player_name.family_reading = read_name();
    config_.player_name.given_reading = read_name();
    config_.player_name.nickname = read_name();
    config_.player_name.nickname_reading = read_name();
    th2::save_config(config_path_, config_);
    return true;
}

void Game::write_u32(std::ostream& out, std::uint32_t value) const
{
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
    out.put(static_cast<char>((value >> 16) & 0xFF));
    out.put(static_cast<char>((value >> 24) & 0xFF));
}

void Game::write_i32(std::ostream& out, std::int32_t value) const
{
    write_u32(out, static_cast<std::uint32_t>(value));
}

void Game::write_i64(std::ostream& out, std::int64_t value) const
{
    write_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFF));
    write_u32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFF));
}

void Game::write_str(std::ostream& out, std::string_view str,
               std::size_t padded_size) const
{
    const auto len = std::min(str.size(), padded_size);
    out.write(str.data(), static_cast<std::streamsize>(len));
    for (std::size_t i = len; i < padded_size; ++i) {
        out.put('\0');
    }
}

std::uint32_t Game::read_u32(std::istream& in) const
{
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(in.get())) << shift;
    }
    return value;
}

std::int32_t Game::read_i32(std::istream& in) const
{
    return static_cast<std::int32_t>(read_u32(in));
}

std::int64_t Game::read_i64(std::istream& in) const
{
    const auto low = static_cast<std::int64_t>(read_u32(in));
    const auto high = static_cast<std::int64_t>(read_u32(in));
    return low | (high << 32);
}

std::string Game::read_str(std::istream& in, std::size_t size) const
{
    std::string result(size, '\0');
    in.read(result.data(), static_cast<std::streamsize>(size));
    const auto null_pos = result.find('\0');
    if (null_pos != std::string::npos) {
        result.resize(null_pos);
    }
    return result;
}


}  // namespace th2app
