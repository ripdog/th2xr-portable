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

bool Game::title_extras_available() const
{
    return runtime_.game_flag(80) != 0 || runtime_.game_flag(87) != 0;
}

bool Game::title_item_disabled(int item) const
{
    if (title_extras_) {
        return item == 3;
    }
    return item == 3 && !title_extras_available();
}

void Game::draw_omake_cancel(int highlight)
{
    if (!ui_sys_cancel_) {
        return;
    }
    const SDL_FRect source{
        highlight ? 188.0f : 0.0f, 0.0f, 188.0f, 32.0f};
    const SDL_FRect destination{306.0f, 496.0f, 188.0f, 32.0f};
    SDL_RenderTexture(
        renderer_, ui_sys_cancel_.get(), &source, &destination);
}

void Game::open_cg_gallery()
{
    begin_transition(1, 15, 128, false);
    omake_cg_entries_.clear();
    omake_cg_thumbnails_.clear();
    OmakeCgEntry entry;
    int first_layout_cg = 0;
    for (const int encoded_cg : cg_gallery_layout) {
        if (encoded_cg != 0) {
            if (first_layout_cg == 0) {
                first_layout_cg = encoded_cg;
                entry.hcg = encoded_cg >= 100000;
            }
            const int cg = encoded_cg % 100000;
            const auto& unlocked = entry.hcg
                ? unlocked_h_cgs_
                : unlocked_visual_cgs_;
            if (unlocked.contains(cg)) {
                entry.variants.push_back(cg);
            }
            continue;
        }

        Texture thumbnail;
        if (!entry.variants.empty()) {
            const auto name = std::format(
                "t{}0{:04d}{}.tga", entry.hcg ? 'h' : 'v',
                entry.variants.front() / 10, first_layout_cg % 10);
            thumbnail = load_texture(renderer_, graphics_, name);
        }
        omake_cg_entries_.push_back(std::move(entry));
        omake_cg_thumbnails_.push_back(std::move(thumbnail));
        entry = {};
        first_layout_cg = 0;
    }
    omake_page_ = 0;
    omake_highlight_ = 0;
    omake_cg_view_.reset();
    omake_cg_variant_ = 0;
    omake_cg_tall_scrolled_ = false;
    omake_cg_full_.reset();
    omake_cg_previous_full_.reset();
    omake_cg_phase_ = OmakeCgPhase::viewing;
    play_bgm(10, true, 255);
    ui_mode_ = UiMode::cg_gallery;
}

void Game::open_music_room()
{
    begin_transition(1, 30, 128, false);
    bgm_.fade_to(0.0f, std::chrono::milliseconds(500), true);
    bgm_track_ = -1;
    omake_highlight_ = 40;
    omake_music_playing_slot_ = -1;
    ui_mode_ = UiMode::music_room;
}

void Game::open_replay_gallery()
{
    begin_transition(1, 30, 128, false);
    for (int slot = 0; slot < 9; ++slot) {
        omake_replay_thumbnails_[slot].reset();
        if (!unlocked_replays_.contains(replay_flags[slot])) {
            continue;
        }
        const auto name =
            std::format("th0{:05d}.tga", replay_thumbnails[slot]);
        if (graphics_.find(name)) {
            omake_replay_thumbnails_[slot] =
                load_texture(renderer_, graphics_, name);
        }
    }
    omake_highlight_ = 14;
    ui_mode_ = UiMode::replay_gallery;
}

void Game::close_omake_screen()
{
    begin_transition(
        1, ui_mode_ == UiMode::cg_gallery ? 15 : 30, 128, false);
    bgm_.fade_to(0.0f, std::chrono::milliseconds(1000), true);
    bgm_track_ = -1;
    omake_cg_full_.reset();
    omake_cg_previous_full_.reset();
    omake_cg_view_.reset();
    ui_mode_ = UiMode::title;
    title_extras_ = true;
    title_highlight_ = 0;
}

void Game::draw_cg_gallery_page()
{
    if (omake_cg_background_) {
        SDL_RenderTexture(
            renderer_, omake_cg_background_.get(), nullptr, nullptr);
    }
    const int first = omake_page_ * 12;
    for (int slot = 0; slot < 12; ++slot) {
        const int index = first + slot;
        const SDL_FRect destination{
            55.0f + 177.0f * (slot % 4),
            114.0f + 128.0f * (slot / 4), 160.0f, 120.0f};
        if (index < static_cast<int>(omake_cg_thumbnails_.size())
            && !omake_cg_entries_[index].variants.empty()) {
            auto* texture = omake_cg_thumbnails_[index].get();
            SDL_SetTextureAlphaMod(
                texture, omake_highlight_ == slot ? 255 : 192);
            SDL_RenderTexture(renderer_, texture, nullptr, &destination);
            SDL_SetTextureAlphaMod(texture, 255);
        } else if (omake_cg_locked_) {
            SDL_RenderTexture(
                renderer_, omake_cg_locked_.get(), nullptr, &destination);
        }
    }
    const int page_count = std::max(
        1, (static_cast<int>(omake_cg_entries_.size()) + 11) / 12);
    font_.draw(
        renderer_, 364.0f, 78.0f,
        std::format("{:02d}/{:02d}", omake_page_ + 1, page_count),
        255, 245, 225);
    if (ui_save_controls_) {
        const SDL_FRect previous_source{
            0.0f, omake_highlight_ == 12 ? 64.0f : 0.0f,
            130.0f, 32.0f};
        const SDL_FRect next_source{
            0.0f, 32.0f + (omake_highlight_ == 13 ? 64.0f : 0.0f),
            130.0f, 32.0f};
        const SDL_FRect previous_destination{
            190.0f, 72.0f, 130.0f, 32.0f};
        const SDL_FRect next_destination{
            482.0f, 72.0f, 130.0f, 32.0f};
        SDL_RenderTexture(
            renderer_, ui_save_controls_.get(),
            &previous_source, &previous_destination);
        SDL_RenderTexture(
            renderer_, ui_save_controls_.get(),
            &next_source, &next_destination);
    }
    draw_omake_cancel(omake_highlight_ == 14);
}

void Game::draw_cg_full(
    SDL_Texture* texture, float source_y, const SDL_FRect& destination,
    float alpha)
{
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(texture, &width, &height);
    SDL_SetTextureAlphaModFloat(texture, alpha);
    if (height > 600.0f) {
        const SDL_FRect source{
            0.0f, source_y, std::min(800.0f, width), 600.0f};
        SDL_RenderTexture(renderer_, texture, &source, &destination);
    } else {
        SDL_RenderTexture(renderer_, texture, nullptr, &destination);
    }
    SDL_SetTextureAlphaModFloat(texture, 1.0f);
}

float Game::omake_cg_phase_progress(int frames) const
{
    return std::clamp(
        static_cast<float>(
            std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                - omake_cg_phase_started_).count()
            * 60.0 / frames),
        0.0f, 1.0f);
}

void Game::draw_cg_gallery()
{
    if (!omake_cg_view_ || !omake_cg_full_) {
        draw_cg_gallery_page();
        return;
    }

    float height = 0.0f;
    SDL_GetTextureSize(omake_cg_full_.get(), nullptr, &height);
    const float bottom = std::max(0.0f, height - 600.0f);
    const SDL_FRect full_screen{0.0f, 0.0f, 800.0f, 600.0f};

    switch (omake_cg_phase_) {
    case OmakeCgPhase::opening: {
        draw_cg_gallery_page();
        const float linear = omake_cg_phase_progress(15);
        const float rate = 1.0f - (1.0f - linear) * (1.0f - linear);
        const int slot = *omake_cg_view_ % 12;
        const SDL_FRect destination{
            (54.0f + 177.0f * (slot % 4)) * (1.0f - rate),
            (77.0f + 130.0f * (slot / 4)) * (1.0f - rate),
            160.0f * (1.0f - rate) + 800.0f * rate,
            120.0f * (1.0f - rate) + 600.0f * rate};
        draw_cg_full(
            omake_cg_full_.get(), bottom, destination, linear);
        if (linear >= 1.0f) {
            omake_cg_phase_ = OmakeCgPhase::viewing;
        }
        break;
    }
    case OmakeCgPhase::scrolling: {
        const float progress = omake_cg_phase_progress(180);
        draw_cg_full(
            omake_cg_full_.get(), bottom * (1.0f - progress),
            full_screen);
        if (progress >= 1.0f) {
            omake_cg_tall_scrolled_ = true;
            omake_cg_phase_ = OmakeCgPhase::viewing;
        }
        break;
    }
    case OmakeCgPhase::changing: {
        draw_cg_full(
            omake_cg_full_.get(), bottom, full_screen);
        const float progress = omake_cg_phase_progress(15);
        if (omake_cg_previous_full_) {
            float previous_height = 0.0f;
            SDL_GetTextureSize(
                omake_cg_previous_full_.get(),
                nullptr, &previous_height);
            draw_cg_full(
                omake_cg_previous_full_.get(),
                std::max(0.0f, previous_height - 600.0f),
                full_screen, 1.0f - progress);
        }
        if (progress >= 1.0f) {
            omake_cg_previous_full_.reset();
            omake_cg_phase_ = OmakeCgPhase::viewing;
        }
        break;
    }
    case OmakeCgPhase::closing: {
        draw_cg_gallery_page();
        const float progress = omake_cg_phase_progress(15);
        draw_cg_full(
            omake_cg_full_.get(),
            omake_cg_tall_scrolled_ ? 0.0f : bottom,
            full_screen, 1.0f - progress);
        if (progress >= 1.0f) {
            omake_cg_view_.reset();
            omake_cg_full_.reset();
            omake_cg_previous_full_.reset();
            omake_cg_phase_ = OmakeCgPhase::viewing;
        }
        break;
    }
    case OmakeCgPhase::viewing:
        draw_cg_full(
            omake_cg_full_.get(),
            omake_cg_tall_scrolled_ ? 0.0f : bottom,
            full_screen);
        break;
    }
}

void Game::draw_music_room()
{
    if (omake_music_background_) {
        SDL_RenderTexture(
            renderer_, omake_music_background_.get(), nullptr, nullptr);
    }
    for (int slot = 0; slot < 40; ++slot) {
        if (runtime_.game_flag(128 + slot) == 0 || !omake_music_labels_) {
            continue;
        }
        const int column = slot / 10;
        const int row = slot % 10;
        const SDL_FRect source{
            static_cast<float>(column * 160),
            static_cast<float>(row * 20), 160.0f, 20.0f};
        const SDL_FRect destination{
            static_cast<float>(40 + column * 192),
            static_cast<float>(164 + row * 32), 160.0f, 20.0f};
        SDL_RenderTexture(
            renderer_, omake_music_labels_.get(), &source, &destination);
    }
    if (omake_highlight_ >= 0 && omake_highlight_ < 40
        && runtime_.game_flag(128 + omake_highlight_) != 0
        && omake_music_selection_) {
        const int column = omake_highlight_ / 10;
        const int row = omake_highlight_ % 10;
        const SDL_FRect source{
            static_cast<float>(20 + column * 192),
            static_cast<float>(160 + row * 32), 184.0f, 28.0f};
        const SDL_FRect destination = source;
        SDL_RenderTexture(
            renderer_, omake_music_selection_.get(),
            &source, &destination);
    }
    if (omake_music_playing_slot_ >= 0 && omake_music_playing_) {
        const int column = omake_music_playing_slot_ / 10;
        const int row = omake_music_playing_slot_ % 10;
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(
            omake_music_playing_.get(), &width, &height);
        const SDL_FRect marker{
            static_cast<float>(26 + column * 192),
            static_cast<float>(164 + row * 32), width, height};
        SDL_RenderTexture(
            renderer_, omake_music_playing_.get(), nullptr, &marker);
    }
    if (omake_music_playing_slot_ >= 0) {
        const int slot = omake_music_playing_slot_;
        if (omake_music_title_) {
            const SDL_FRect source{
                0.0f, static_cast<float>(slot * 24),
                282.0f, 24.0f};
            const SDL_FRect destination{
                281.0f, 86.0f, 282.0f, 24.0f};
            SDL_RenderTexture(
                renderer_, omake_music_title_.get(),
                &source, &destination);
        }
        if (omake_music_artist_) {
            const SDL_FRect composer_source{
                0.0f,
                static_cast<float>(music_room_artists[slot][0] * 24),
                282.0f, 24.0f};
            const SDL_FRect arranger_source{
                0.0f,
                static_cast<float>(music_room_artists[slot][1] * 24),
                282.0f, 24.0f};
            const SDL_FRect composer_destination{
                291.0f, 122.0f, 282.0f, 24.0f};
            const SDL_FRect arranger_destination{
                440.0f, 122.0f, 282.0f, 24.0f};
            SDL_RenderTexture(
                renderer_, omake_music_artist_.get(),
                &composer_source, &composer_destination);
            SDL_RenderTexture(
                renderer_, omake_music_artist_.get(),
                &arranger_source, &arranger_destination);
        }
    }
    draw_omake_cancel(omake_highlight_ == 40);
}

void Game::draw_replay_gallery()
{
    if (omake_replay_background_) {
        SDL_RenderTexture(
            renderer_, omake_replay_background_.get(), nullptr, nullptr);
    }
    for (int slot = 0; slot < 9; ++slot) {
        const SDL_FRect destination{
            55.0f + 177.0f * (slot % 4),
            114.0f + 128.0f * (slot / 4), 160.0f, 120.0f};
        const bool unlocked =
            unlocked_replays_.contains(replay_flags[slot]);
        if (unlocked && omake_replay_thumbnails_[slot]) {
            SDL_SetTextureAlphaMod(
                omake_replay_thumbnails_[slot].get(),
                omake_highlight_ == slot ? 255 : 192);
            SDL_RenderTexture(
                renderer_, omake_replay_thumbnails_[slot].get(),
                nullptr, &destination);
        } else if (omake_cg_locked_) {
            SDL_RenderTexture(
                renderer_, omake_cg_locked_.get(), nullptr, &destination);
        }
    }
    draw_omake_cancel(omake_highlight_ == 14);
}

void Game::draw_title()
{
    const auto now = std::chrono::steady_clock::now();
    const int frame = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - title_started_).count() * 60 / 1000);
    float brightness = 1.0f;
    if (title_exit_started_) {
        const int exit_frame = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *title_exit_started_).count() * 60 / 1000);
        brightness = std::clamp(
            1.0f - static_cast<float>(exit_frame) / 16.0f, 0.0f, 1.0f);
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (movie_) {
        movie_->draw();
        return;
    }
    if (title_background_) {
        SDL_SetTextureColorModFloat(
            title_background_.get(), brightness, brightness, brightness);
        SDL_SetTextureAlphaModFloat(title_background_.get(),
            std::clamp(static_cast<float>(frame) / 16.0f, 0.0f, 1.0f));
        SDL_RenderTexture(renderer_, title_background_.get(), nullptr, nullptr);
    }
    if (title_foreground_pixels_ && title_masked_ && frame >= 16) {
        const int vague = 32;
        const int rate = std::clamp((frame - 16) * 8, 0, 256);
        const int blend_offset = rate * (256 + vague) / 256;
        std::vector<std::uint8_t> pixels(800 * 600 * 4);
        const auto* source = static_cast<const std::uint8_t*>(
            title_foreground_pixels_->pixels);
        for (int y = 0; y < 600; ++y) {
            const int mask_y = y * title_mask_height_ / 600;
            for (int x = 0; x < 800; ++x) {
                const int mask_x = x * title_mask_width_ / 800;
                const int mask = title_mask_[
                    static_cast<std::size_t>(mask_y) * title_mask_width_
                    + mask_x];
                const int alpha = std::clamp(
                    (mask + blend_offset - 256) * 256 / vague, 0, 255);
                const auto source_offset =
                    static_cast<std::size_t>(y)
                        * title_foreground_pixels_->pitch
                    + static_cast<std::size_t>(x) * 4;
                const auto output_offset =
                    (static_cast<std::size_t>(y) * 800 + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    pixels[output_offset + channel] =
                        source[source_offset + channel];
                }
                pixels[output_offset + 3] =
                    static_cast<std::uint8_t>(alpha);
            }
        }
        SDL_UpdateTexture(
            title_masked_.get(), nullptr, pixels.data(), 800 * 4);
        SDL_SetTextureColorModFloat(
            title_masked_.get(), brightness, brightness, brightness);
        SDL_RenderTexture(renderer_, title_masked_.get(), nullptr, nullptr);
    }
    if (!title_menu_ || frame < 48) {
        return;
    }

    SDL_SetTextureColorModFloat(
        title_menu_.get(), brightness, brightness, brightness);
    SDL_SetTextureAlphaModFloat(title_menu_.get(), 1.0f);
    const SDL_FRect logo_src{564.0f, 0.0f, 177.0f, 38.0f};
    const SDL_FRect logo_dst{477.0f, 304.0f, 177.0f, 38.0f};
    SDL_RenderTexture(renderer_, title_menu_.get(), &logo_src, &logo_dst);
    if (frame >= 80 && frame < 88) {
        const float flare = 1.0f - static_cast<float>(frame - 80) / 8.0f;
        const float zoom = static_cast<float>(frame - 80) * 8.0f / 256.0f;
        SDL_SetTextureBlendMode(title_menu_.get(), SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaModFloat(title_menu_.get(), flare);
        const SDL_FRect flare_dst{
            logo_dst.x - logo_dst.w * zoom / 2.0f,
            logo_dst.y - logo_dst.h * zoom / 2.0f,
            logo_dst.w * (1.0f + zoom),
            logo_dst.h * (1.0f + zoom)};
        SDL_RenderTexture(
            renderer_, title_menu_.get(), &logo_src, &flare_dst);
        SDL_SetTextureBlendMode(title_menu_.get(), SDL_BLENDMODE_BLEND);
    }

    const auto draw_menu_page =
        [&](bool extras, float transition_tick, bool target_page) {
            for (int i = 0; i < 5; ++i) {
                if (i == 3
                    && (extras || !title_extras_available())) {
                    continue;
                }
                float alpha = std::clamp(
                    static_cast<float>(frame - 48 - 40 - i * 4)
                        / 16.0f,
                    0.0f, 1.0f);
                if (transition_tick >= 0.0f) {
                    const float row_progress = std::clamp(
                        (transition_tick - i * 2.0f) / 16.0f,
                        0.0f, 1.0f);
                    alpha = target_page
                        ? row_progress : 1.0f - row_progress;
                }
                SDL_SetTextureAlphaModFloat(title_menu_.get(), alpha);
                const float source_x =
                    target_page && i == title_highlight_
                    && !title_menu_transition_started_
                        ? 188.0f : 0.0f;
                const SDL_FRect source{
                    source_x,
                    static_cast<float>(32 * (i + (extras ? 5 : 0))),
                    188.0f, 32.0f};
                const SDL_FRect destination{
                    306.0f, static_cast<float>(385 + 40 * i),
                    188.0f, 32.0f};
                SDL_RenderTexture(
                    renderer_, title_menu_.get(),
                    &source, &destination);
            }
        };
    if (title_menu_transition_started_) {
        const float tick = static_cast<float>(
            std::chrono::duration<double>(
                now - *title_menu_transition_started_).count() * 60.0);
        draw_menu_page(
            title_extras_transition_from_, tick, false);
        draw_menu_page(title_extras_, tick, true);
    } else {
        draw_menu_page(title_extras_, -1.0f, true);
    }
    SDL_SetTextureAlphaModFloat(title_menu_.get(), 1.0f);
}

void Game::activate_title_item()
{
    play_se(
        -1, !title_extras_ && title_highlight_ == 0 ? 9111 : 9104,
        false, 255);
    if (title_extras_) {
        switch (title_highlight_) {
        case 0:
            open_cg_gallery();
            break;
        case 1:
            open_music_room();
            break;
        case 2:
            open_replay_gallery();
            break;
        case 4:
            begin_title_menu_transition(false);
            title_highlight_ = 3;
            break;
        default:
            break;
        }
        return;
    }
    switch (title_highlight_) {
    case 0:
        begin_title_exit(true);
        break;
    case 1:
        save_snapshot_ = capture_frame_pixels();
        open_save_load(UiMode::load);
        break;
    case 2:
        open_config();
        break;
    case 3:
        begin_title_menu_transition(true);
        title_highlight_ = 0;
        break;
    case 4:
        begin_title_exit(false);
        break;
    default:
        break;
    }
}


}  // namespace th2app
