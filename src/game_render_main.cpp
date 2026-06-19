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

void Game::draw_click_indicator()
{
    if (!waiting_for_input_ || !message_visible_ || message_.empty()
        || !text_reveal_complete_) {
        return;
    }

    const bool end_of_block =
        !message_.has_hidden_segments() && message_ends_block_;
    auto& tex = end_of_block ? ui_keywait_ : ui_pageend_;
    if (!tex) return;

    const auto lines = display_lines(message_.visible());
    if (lines.empty()) return;
    const auto& last_line = lines.back();
    const float width = font_.text_width(last_line);

    const float x = message_text_x() + width + 4.0f;
    const float y = message_text_y()
        + (std::min(lines.size(), static_cast<std::size_t>(15)) - 1)
        * text_line_height() - 2.0f;

    // Time-based 30fps animation matching original GlobalCount/2%30 (1s cycle)
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int frame = (ms / 33) % 30;
    const SDL_FRect src{frame * 40.0f, 0.0f, 40.0f, 40.0f};
    const SDL_FRect dst{x, y, 36.0f, 36.0f};
    SDL_RenderTexture(renderer_, tex.get(), &src, &dst);
}

void Game::select_overlay()
{
    auto* overlay = upscaler_->overlay_target();
    SDL_SetRenderTarget(renderer_, overlay);
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(overlay, &width, &height);
    SDL_SetRenderScale(renderer_, width / 800.0f, height / 600.0f);
}

void Game::begin_overlay()
{
    select_overlay();
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::select_sidebar()
{
    auto* sidebar = upscaler_->sidebar_target();
    SDL_SetRenderTarget(renderer_, sidebar);
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(sidebar, &width, &height);
    SDL_SetRenderScale(renderer_, width / 800.0f, height / 600.0f);
}

void Game::clear_sidebar()
{
    select_sidebar();
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::clear_authentic_text()
{
    SDL_SetRenderTarget(renderer_, upscaler_->authentic_text_target());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

void Game::begin_authentic_text()
{
    SDL_SetRenderTarget(renderer_, upscaler_->authentic_text_target());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
}

void Game::draw_overlay(std::size_t slot)
{
    if (!overlays_[slot] || !overlay_states_[slot].visible) {
        return;
    }
    const auto& state = overlay_states_[slot];
    auto* texture = overlays_[slot].get();
    const int alpha = state.parameter == 11
        ? std::clamp(state.parameter_value, 0, 256) * 255 / 256
        : 255;
    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));
    if (state.parameter == 1) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    } else if (state.parameter == 5) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_MOD);
    } else {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }

    const auto scale_x = [](int value) {
        return value * 800.0f / 640.0f;
    };
    const auto scale_y = [](int value) {
        return value * 600.0f / 448.0f;
    };
    SDL_FRect source{
        scale_x(state.source_x), scale_y(state.source_y),
        scale_x(state.source_width), scale_y(state.source_height)};
    SDL_FRect destination{
        scale_x(state.destination_x), scale_y(state.destination_y),
        scale_x(state.destination_width),
        scale_y(state.destination_height)};
    if (state.zoom != 0) {
        const float scale = (256.0f + state.zoom) / 256.0f;
        const float center_x = scale_x(state.zoom_center_x);
        const float center_y = scale_y(state.zoom_center_y);
        destination.x = center_x + (destination.x - center_x) * scale;
        destination.y = center_y + (destination.y - center_y) * scale;
        destination.w *= scale;
        destination.h *= scale;
    }
    SDL_FlipMode flip = SDL_FLIP_NONE;
    if ((state.reverse & 0x10) != 0) {
        flip = static_cast<SDL_FlipMode>(flip | SDL_FLIP_HORIZONTAL);
    }
    if ((state.reverse & 0x20) != 0) {
        flip = static_cast<SDL_FlipMode>(flip | SDL_FLIP_VERTICAL);
    }
    SDL_RenderTextureRotated(
        renderer_, texture, &source, &destination, 0.0, nullptr, flip);
}

float Game::imgui_display_scale() const
{
    // Cap the scale so ImGui doesn't become enormous on high-DPI phones.
    // 2.5x is plenty for crisp text on 5K+ desktop displays and keeps
    // phone UIs usable.
    return std::clamp(SDL_GetWindowDisplayScale(window_), 1.0f, 2.5f);
}

void Game::present_frame()
{
    upscaler_->present();

    // ImGui is rendered directly to the window backbuffer using a capped
    // display scale, so the debug/config UI stays crisp but does not
    // balloon to the full screen magnification used for the 800x600 art.
    const float display_scale = imgui_display_scale();
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderScale(renderer_, display_scale, display_scale);
    imgui_->render();

    SDL_RenderPresent(renderer_);
}

void Game::reset_render_state()
{
    SDL_Log("Resetting render state after resume/reset");
    if (upscaler_) {
        upscaler_->reset();
    }
    shake_target_.reset();
    title_masked_.reset();
    if (imgui_) {
        imgui_->rebuild_font_atlas(imgui_display_scale());
    }
}

void Game::ensure_shake_target()
{
    if (shake_target_) {
        return;
    }
    shake_target_.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET, 800, 600));
    if (!shake_target_) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(shake_target_.get(), SDL_BLENDMODE_NONE);
}

void Game::draw_frame()
{
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_Texture* art_target = upscaler_->art_target();
    const auto shake = shake_sample();
    const bool shake_background = shake_
        && (shake_->type == 0 || shake_->type == 1
            || shake_->type == 2 || shake_->type == 9
            || shake_->type == 12 || shake_->type == 13
            || shake_->type == 14);
    const bool shake_characters = shake_ && shake_->type == 0;
    const bool shake_art = shake_
        && (shake_->type == 6 || shake_->type == 7
            || shake_->type == 11 || shake_->type == 16);
    if (shake_art) {
        ensure_shake_target();
        SDL_SetRenderTarget(renderer_, shake_target_.get());
    } else {
        SDL_SetRenderTarget(renderer_, art_target);
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (movie_) {
        movie_->draw();
        begin_overlay();

        present_frame();
        return;
    }
    if (name_input_open_) {
        begin_overlay();

        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::title) {
        draw_title();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::cg_gallery) {
        draw_cg_gallery();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::music_room) {
        draw_music_room();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::replay_gallery) {
        draw_replay_gallery();
        draw_active_transition();
        begin_overlay();
        present_frame();
        return;
    }
    if (ui_mode_ == UiMode::map) {
        draw_map(false);
        begin_overlay();
        draw_map(true);
        draw_script_position();

        present_frame();
        return;
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer < 1) {
            draw_overlay(i);
        }
    }
    if (background_) {
        const auto view = current_background_view();
        SDL_FRect source{
            view.x, view.y, view.width, view.height};
        SDL_FRect destination{0.0f, 0.0f, 800.0f, 600.0f};
        double angle = 0.0;
        if (shake_background) {
            destination = {
                -shake.x + 400.0f * (1.0f - shake.scale),
                -shake.y + 300.0f * (1.0f - shake.scale),
                800.0f * shake.scale,
                600.0f * shake.scale,
            };
            angle = shake.angle;
        }
        if (clip_texture_source(
                background_.get(), source, destination)) {
            SDL_RenderTextureRotated(
                renderer_, background_.get(), &source, &destination,
                angle, nullptr, SDL_FLIP_NONE);
        }
    } else if (bg_scene_ == 0) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
        SDL_RenderFillRect(renderer_, &game_area);
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer >= 1
            && overlay_states_[i].layer < 5) {
            draw_overlay(i);
        }
    }
    if (background_brightness_ != std::array<float, 3>{
            128.0f, 128.0f, 128.0f}) {
        const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
        std::array<Uint8, 3> multiply{};
        std::array<Uint8, 3> screen{};
        bool needs_multiply = false;
        bool needs_screen = false;
        for (std::size_t i = 0; i < background_brightness_.size(); ++i) {
            const float value =
                std::clamp(background_brightness_[i], 0.0f, 256.0f);
            multiply[i] = static_cast<Uint8>(
                value < 128.0f ? value * 255.0f / 128.0f : 255.0f);
            screen[i] = static_cast<Uint8>(
                value > 128.0f
                    ? (value - 128.0f) * 255.0f / 128.0f
                    : 0.0f);
            needs_multiply |= value < 128.0f;
            needs_screen |= value > 128.0f;
        }
        if (needs_multiply) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_MOD);
            SDL_SetRenderDrawColor(
                renderer_, multiply[0], multiply[1], multiply[2], 255);
            SDL_RenderFillRect(renderer_, &game_area);
        }
        if (needs_screen) {
            const auto screen_blend = SDL_ComposeCustomBlendMode(
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR,
                SDL_BLENDOPERATION_ADD,
                SDL_BLENDFACTOR_ZERO,
                SDL_BLENDFACTOR_ONE,
                SDL_BLENDOPERATION_ADD);
            SDL_SetRenderDrawBlendMode(renderer_, screen_blend);
            SDL_SetRenderDrawColor(
                renderer_, screen[0], screen[1], screen[2], 0);
            SDL_RenderFillRect(renderer_, &game_area);
        }
    }
    draw_sakura();
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer >= 5
            && overlay_states_[i].layer < 18) {
            draw_overlay(i);
        }
    }
    for (const auto& character : characters_.ordered()) {
        if (character_staged_.at(character.number)) {
            continue;
        }
        auto& loaded = character_texture(character.number);
        if (!loaded.texture) {
            continue;
        }
        auto& animation = character_animations_.at(character.number);
        float progress = 1.0f;
        if (animation.kind != CharacterAnimationKind::none) {
            progress = std::clamp(
                static_cast<float>(std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - animation.started).count() * 60.0
                    / animation.frames),
                0.0f, 1.0f);
        }
        const float eased = 1.0f
            - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
        float x = static_cast<float>(
            th2::character_offset(character.locate));
        int brightness_value = character.brightness;
        int alpha_value = character.alpha;
        if (animation.kind == CharacterAnimationKind::enter) {
            if (animation.type == 1 || animation.type == 2) {
                const float start = animation.type == 1 ? -600.0f : 600.0f;
                x = start + (x - start) * eased;
            } else {
                alpha_value = static_cast<int>(
                    animation.to_alpha * progress);
            }
        } else if (animation.kind == CharacterAnimationKind::leave) {
            if (animation.type == 1 || animation.type == 2) {
                const float destination =
                    animation.type == 1 ? -600.0f : 600.0f;
                x += (destination - x) * progress * progress * progress;
            } else {
                alpha_value = static_cast<int>(
                    animation.from_alpha * (1.0f - progress));
            }
        } else if (animation.kind == CharacterAnimationKind::locate) {
            const float from = static_cast<float>(
                th2::character_offset(animation.from_locate));
            const float to = static_cast<float>(
                th2::character_offset(animation.to_locate));
            x = from + (to - from) * eased;
        } else if (animation.kind
                   == CharacterAnimationKind::brightness) {
            brightness_value = static_cast<int>(
                animation.from_brightness
                + (animation.to_brightness
                   - animation.from_brightness) * progress);
        } else if (animation.kind == CharacterAnimationKind::alpha) {
            alpha_value = static_cast<int>(
                animation.from_alpha
                + (animation.to_alpha - animation.from_alpha) * progress);
        } else if (animation.kind == CharacterAnimationKind::pose) {
            alpha_value = static_cast<int>(
                animation.to_alpha * progress);
        }
        const auto brightness = static_cast<Uint8>(
            std::clamp(brightness_value * 2, 0, 255));
        SDL_SetTextureColorMod(
            loaded.texture.get(), brightness, brightness, brightness);
        SDL_SetTextureAlphaMod(
            loaded.texture.get(),
            static_cast<Uint8>(
                std::clamp(alpha_value, 0, 256) * 255 / 256));
        SDL_FRect destination{
            x + (shake_characters ? shake.x : 0.0f),
            shake_characters ? shake.y : 0.0f,
            800.0f, 600.0f};
        if (animation.kind == CharacterAnimationKind::pose
            && animation.previous) {
            SDL_SetTextureColorMod(
                animation.previous.get(),
                brightness, brightness, brightness);
            SDL_SetTextureAlphaMod(
                animation.previous.get(),
                static_cast<Uint8>(
                    std::clamp(static_cast<int>(
                        animation.from_alpha * (1.0f - progress)),
                        0, 256) * 255 / 256));
            SDL_RenderTexture(
                renderer_, animation.previous.get(), nullptr, &destination);
        }
        SDL_RenderTexture(renderer_, loaded.texture.get(), nullptr, &destination);
    }
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        if (overlay_states_[i].layer >= 18) {
            draw_overlay(i);
        }
    }
    if (ui_mode_ == UiMode::system_menu) {
        draw_system_menu();
    } else if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
        draw_save_load();
    }
    draw_active_transition();
    if (shake_art) {
        SDL_SetRenderTarget(renderer_, art_target);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_FRect destination{
            shake.x + 400.0f * (1.0f - shake.scale),
            shake.y + 300.0f * (1.0f - shake.scale),
            800.0f * shake.scale, 600.0f * shake.scale};
        SDL_RenderTextureRotated(
            renderer_, shake_target_.get(), nullptr, &destination,
            shake.angle, nullptr, SDL_FLIP_NONE);
    }
    clear_authentic_text();
    clear_sidebar();
    begin_overlay();
    if (clock_state_ || calendar_state_) {
        draw_clock_calendar();
        draw_script_position();

        present_frame();
        return;
    }
    if (shake_ && (shake.text_only || shake.includes_text)) {
        const SDL_Rect viewport{
            static_cast<int>(shake.x), static_cast<int>(shake.y),
            800, 600};
        SDL_SetRenderViewport(renderer_, &viewport);
    }
    if (ui_mode_ == UiMode::game
        && message_visible_ && !message_.empty()) {
        if (font_.authentic()) {
            begin_authentic_text();
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 16, 150);
        SDL_RenderFillRect(renderer_, nullptr);
        const auto visible = message_.visible();
        const auto reveal_start =
            std::min(text_reveal_start_, visible.size());
        const auto reveal_text = visible.substr(reveal_start);
        const auto reveal_character_count =
            utf8_character_count(reveal_text);
        float reveal_position =
            static_cast<float>(reveal_character_count);
        if (!text_reveal_complete_ && config_.text_speed_ms > 0) {
            const auto elapsed =
                std::chrono::steady_clock::now() - text_reveal_started_;
            reveal_position =
                std::chrono::duration<float, std::milli>(elapsed).count()
                / config_.text_speed_ms;
            if (reveal_position >= reveal_character_count + 8.0f) {
                text_reveal_complete_ = true;
                reveal_position =
                    static_cast<float>(reveal_character_count);
            }
        }
        constexpr float fade_width = 16.0f;
        const float x = message_text_x();
        float y = message_text_y();
        std::size_t source_cursor = 0;
        const auto lines = display_lines(visible);
        for (const auto& line : lines) {
            auto line_start = visible.find(line, source_cursor);
            if (line_start == std::string_view::npos) {
                line_start = source_cursor;
            }
            std::size_t glyph_offset = 0;
            float authentic_x = x;
            while (glyph_offset < line.size()) {
                const auto glyph_bytes = utf8_prefix_bytes(
                    std::string_view(line).substr(glyph_offset), 1);
                const auto source_offset = line_start + glyph_offset;
                float glyph_alpha = 1.0f;
                if (!text_reveal_complete_
                    && source_offset >= reveal_start) {
                    const auto glyph_index = utf8_character_count(
                        visible.substr(
                            reveal_start,
                            source_offset - reveal_start));
                    glyph_alpha = std::clamp(
                        reveal_position
                            - static_cast<float>(glyph_index),
                        0.0f, fade_width) / fade_width;
                }
                if (glyph_alpha > 0.0f) {
                    const auto glyph_end = glyph_offset + glyph_bytes;
                    const auto glyph = std::string_view(line).substr(
                        glyph_offset, glyph_bytes);
                    const auto alpha = static_cast<std::uint8_t>(
                        glyph_alpha * 255.0f);
                    if (font_.authentic()) {
                        font_.draw_authentic_shadow(
                            renderer_, authentic_x, y, glyph, alpha);
                        font_.draw(
                            renderer_, authentic_x, y, glyph,
                            255, 255, 255, alpha);
                        authentic_x += font_.text_width(glyph);
                        glyph_offset += glyph_bytes;
                        continue;
                    }
                    const auto prefix =
                        std::string_view(line).substr(0, glyph_offset);
                    const auto through_glyph =
                        std::string_view(line).substr(0, glyph_end);
                    const float glyph_left =
                        x + font_.text_width(prefix);
                    const float glyph_right =
                        x + font_.text_width(through_glyph);
                    const SDL_Rect clip{
                        static_cast<int>(std::floor(glyph_left)),
                        static_cast<int>(std::floor(y)),
                        std::max(
                            1, static_cast<int>(
                                std::ceil(glyph_right - glyph_left))),
                        31};
                    SDL_SetRenderClipRect(renderer_, &clip);
                    font_.draw(
                        renderer_, x + 2.0f, y + 2.0f,
                        line, 0, 0, 0, alpha);
                    font_.draw(
                        renderer_, x, y, line,
                        255, 255, 255, alpha);
                    SDL_SetRenderClipRect(renderer_, nullptr);
                }
                glyph_offset += glyph_bytes;
            }
            source_cursor = line_start + line.size();
            y += text_line_height();
            if (y > 535.0f) {
                break;
            }
        }
        if (font_.authentic()) {
            select_overlay();
        }
    }
    if (ui_mode_ == UiMode::game
        && message_visible_ && choosing_ && !choices_.empty()) {
        float y = choice_y_start();
        for (int i = 0; i < static_cast<int>(choices_.size()); ++i) {
            const auto highlighted = i == choice_highlight_;
            for (const auto& line : choice_lines(choices_[i], i)) {
                font_.draw(
                    renderer_, 34.0f, y + 2.0f, line, 0, 0, 0);
                font_.draw(
                    renderer_, 32.0f, y, line,
                    highlighted ? 255 : 128,
                    highlighted ? 255 : 128,
                    highlighted ? 255 : 128);
                y += text_line_height();
            }
        }
    }
    SDL_SetRenderViewport(renderer_, nullptr);
    if (ui_mode_ == UiMode::game) {
        draw_click_indicator();
    }
    if (ui_mode_ == UiMode::backlog) {
        draw_backlog();
    }
    select_sidebar();
    if ((ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog) && message_visible_) {
        draw_sidebar();
    }
    select_overlay();
    if (screen_flash_) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(
            renderer_,
            static_cast<Uint8>(screen_flash_->red),
            static_cast<Uint8>(screen_flash_->green),
            static_cast<Uint8>(screen_flash_->blue),
            static_cast<Uint8>(screen_flash_alpha() * 255.0f));
        SDL_RenderFillRect(renderer_, nullptr);
    }
    draw_script_position();
    present_frame();
}


}  // namespace th2app
