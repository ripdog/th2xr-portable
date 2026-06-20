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

bool Game::volume_control(const char* label, int& volume, bool& muted)
{
    bool volume_changed = false;
    ImGui::PushID(label);
    ImGui::BeginDisabled(muted);
    volume_changed = ImGui::SliderInt("##volume", &volume, 0, 256);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool mute_changed = ImGui::Checkbox("Mute", &muted);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    if (mute_changed) {
        play_se(-1, 9104, false, 255);
    }
    return volume_changed || mute_changed;
}

void Game::return_to_title()
{
    if (soak_) {
        soak_->finish_route(runtime_.script_name(), runtime_.vm_pc());
    }
    config_open_ = false;
    confirm_return_title_ = false;
    auto_mode_ = false;
    skip_mode_ = false;
    replay_mode_ = false;
    wake_time_.reset();
    audio_wait_.reset();
    transition_.reset();
    background_fade_.reset();
    bgm_.stop();
    for (auto& channel : transient_se_) channel.stop();
    for (auto& channel : se_channels_) channel.stop();
    for (auto& channel : voice_channels_) channel.stop();
    ui_mode_ = UiMode::title;
    title_highlight_ = 0;
    title_extras_ = false;
    title_menu_transition_started_.reset();
    title_exit_started_.reset();
    title_started_ = std::chrono::steady_clock::now();
    th2::save_config(config_path_, config_);
}

void Game::draw_config()
{
    if (!config_open_) {
        return;
    }
#ifdef __ANDROID__
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
#else
    ImGui::SetNextWindowSize(ImVec2(570.0f, 500.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(115.0f, 50.0f), ImGuiCond_FirstUseEver);
#endif
    bool open = true;
#ifdef __ANDROID__
    constexpr auto config_flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoCollapse;
#else
    constexpr auto config_flags = ImGuiWindowFlags{};
#endif
    if (ImGui::Begin("Panel Config", &open, config_flags)) {
#ifdef __ANDROID__
        ImGui::BeginChild(
            "config_scroll", ImVec2(0, 0), 0,
            ImGuiWindowFlags_NoMove);
#endif
        bool option_changed = false;
        if (ImGui::BeginTabBar("config-tabs")) {
            if (ImGui::BeginTabItem("Playback")) {
                option_changed |= ImGui::Checkbox(
                    "Auto mode skips previously read text",
                    &config_.auto_skip_read);
                ImGui::SliderInt(
                    "Auto delay between lines", &config_.auto_line_ms,
                    250, 10000, "%d ms");
                ImGui::SliderInt(
                    "Auto delay at page end", &config_.auto_page_ms,
                    500, 15000, "%d ms");
                option_changed |= ImGui::SliderInt(
                    "Text speed", &config_.text_speed_ms,
                    0, 100, "%d ms/character");
                ImGui::Separator();
                option_changed |= ImGui::Checkbox(
                    "Auto-skip includes unread text",
                    &config_.skip_unread);
                ImGui::Separator();
                option_changed |= ImGui::Checkbox(
                    "Autosave after text block (2 min interval)",
                    &config_.autosave_enabled);
                ImGui::SeparatorText("Save transfer");
                const bool bundle_dialog_active = [&] {
                    std::lock_guard lock(save_bundle_dialog_.mutex);
                    return save_bundle_dialog_.active;
                }();
                ImGui::BeginDisabled(bundle_dialog_active);
                if (ImGui::Button("Export saves...", ImVec2(150.0f, 0.0f))) {
                    play_se(-1, 9104, false, 255);
                    show_save_bundle_export_dialog();
                }
                ImGui::SameLine();
                if (ImGui::Button("Import saves...", ImVec2(150.0f, 0.0f))) {
                    play_se(-1, 9104, false, 255);
                    show_save_bundle_import_dialog();
                }
                ImGui::EndDisabled();
                if (!save_bundle_status_.empty()) {
                    ImGui::TextWrapped("%s", save_bundle_status_.c_str());
                } else {
                    ImGui::TextDisabled(
                        "Bundles include saves and config .ini files.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                bool audio_changed = volume_control(
                    "Music", config_.bgm_volume, config_.bgm_muted);
                audio_changed |= volume_control(
                    "Sound effects", config_.se_volume, config_.se_muted);
                audio_changed |= volume_control(
                    "Voices", config_.voice_volume, config_.voice_muted);
                ImGui::SeparatorText("Character voices");
                static constexpr std::array labels{
                    "Konomi", "Manaka", "Tamaki", "Karin", "Sango",
                    "Ruri", "Yuma", "Lucy", "Yuki", "Other", "Sasara",
                };
                for (std::size_t i = 0; i < labels.size(); ++i) {
                    audio_changed |= volume_control(
                        labels[i], config_.character_voice_volume[i],
                        config_.character_voice_muted[i]);
                }
                if (audio_changed) {
                    apply_audio_gains();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display & Input")) {
#ifndef __ANDROID__
                if (ImGui::Checkbox("Fullscreen", &config_.fullscreen)) {
                    toggle_fullscreen();
                    option_changed = true;
                }
#endif
                ImGui::BeginDisabled(!anime4k_available_);
                option_changed |= ImGui::Checkbox(
                    "Anime4K art upscaling", &config_.anime4k);
                ImGui::EndDisabled();
                if (!anime4k_available_) {
                    ImGui::TextDisabled(
                        "Anime4K requires SDL's GPU renderer with SPIR-V support.");
                }
                ImGui::SeparatorText("Text");
                option_changed |= ImGui::Checkbox(
                    "Authentic bitmap font", &config_.authentic_font);
                ImGui::BeginDisabled(config_.authentic_font);
                const auto& families =
                    th2::GameFont::system_families();
                if (ImGui::BeginCombo(
                        "Font", config_.font_family.c_str())) {
                    for (const auto& family : families) {
                        const bool selected =
                            family == config_.font_family;
                        if (ImGui::Selectable(
                                family.c_str(), selected)) {
                            config_.font_family = family;
                            option_changed = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                option_changed |= ImGui::InputInt(
                    "Font size", &config_.font_size, 1, 4);
                config_.font_size =
                    std::clamp(config_.font_size, 12, 48);
                ImGui::EndDisabled();
                option_changed |= ImGui::Checkbox(
                    "Mouse wheel opens backlog",
                    &config_.wheel_opens_backlog);
                static constexpr std::array sidebar_modes{
                    "Fade when away", "Always visible",
                    "Disappear when away", "Hidden",
                };
                if (ImGui::BeginCombo(
                        "Sidebar",
                        sidebar_modes[config_.sidebar_mode])) {
                    for (int i = 0;
                         i < static_cast<int>(sidebar_modes.size()); ++i) {
                        const bool selected = config_.sidebar_mode == i;
                        if (ImGui::Selectable(
                                sidebar_modes[i], selected)) {
                            config_.sidebar_mode = i;
                            option_changed = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SeparatorText("Debug");
                option_changed |= ImGui::Checkbox(
                    "Show script position",
                    &config_.show_script_position);
                option_changed |= ImGui::Checkbox(
                    "Dump transition frames",
                    &config_.dump_transition_frames);
                ImGui::TextDisabled(
                    "Dumps are written to debug/transitions/");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (option_changed) {
            play_se(-1, 9104, false, 255);
        }
        ImGui::Separator();
#ifdef __ANDROID__
        if (ImGui::Button("Close", ImVec2(-FLT_MIN, 0.0f))) {
            play_se(-1, 9104, false, 255);
            open = false;
        }
        if (ui_mode_ != UiMode::title
            && ImGui::Button("Return to Title", ImVec2(-FLT_MIN, 0.0f))) {
            play_se(-1, 9104, false, 255);
            confirm_return_title_ = true;
            ImGui::OpenPopup("Return to Title?");
        }
#else
        if (ImGui::Button("Close", ImVec2(110.0f, 0.0f))) {
            play_se(-1, 9104, false, 255);
            open = false;
        }
        if (ui_mode_ != UiMode::title) {
            ImGui::SameLine();
            if (ImGui::Button(
                    "Return to Title", ImVec2(140.0f, 0.0f))) {
                play_se(-1, 9104, false, 255);
                confirm_return_title_ = true;
                ImGui::OpenPopup("Return to Title?");
            }
        }
#endif
        if (ImGui::BeginPopupModal(
                "Return to Title?", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(
                "Unsaved progress will be lost.\nReturn to the title screen?");
#ifdef __ANDROID__
            if (ImGui::Button("Return", ImVec2(-FLT_MIN, 0.0f))) {
                play_se(-1, 9104, false, 255);
                return_to_title();
                ImGui::CloseCurrentPopup();
                open = false;
            }
            if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0.0f))) {
                play_se(-1, 9107, false, 255);
                confirm_return_title_ = false;
                ImGui::CloseCurrentPopup();
            }
#else
            if (ImGui::Button("Return", ImVec2(120.0f, 0.0f))) {
                play_se(-1, 9104, false, 255);
                return_to_title();
                ImGui::CloseCurrentPopup();
                open = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
                play_se(-1, 9107, false, 255);
                confirm_return_title_ = false;
                ImGui::CloseCurrentPopup();
            }
#endif
            ImGui::EndPopup();
        }
#ifdef __ANDROID__
        imgui_->touch_drag_scroll();
        ImGui::EndChild();
#endif
    }
    ImGui::End();
    if (!open) {
        close_config();
    }
}

void Game::draw_name_input()
{
    if (!name_input_open_) {
        return;
    }
#ifdef __ANDROID__
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin(
        "Player Name", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoTitleBar);
    ImGui::BeginChild(
        "name_scroll", ImVec2(0, 0), 0,
        ImGuiWindowFlags_NoMove);
#else
    ImGui::SetNextWindowSize(ImVec2(430.0f, 330.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(400.0f, 300.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin(
        "Player Name", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
#endif
    ImGui::TextUnformatted("Enter the protagonist's name.");
    ImGui::Separator();
#ifdef __ANDROID__
    const auto full_width_input = [&](
        const char* label, char* buf, std::size_t size) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText(label, buf, size);
    };
    full_width_input("Family name", name_family_.data(), name_family_.size());
    full_width_input("Given name", name_given_.data(), name_given_.size());
    full_width_input(
        "Family reading", name_family_reading_.data(),
        name_family_reading_.size());
    full_width_input(
        "Given reading", name_given_reading_.data(),
        name_given_reading_.size());
    full_width_input("Nickname", name_nickname_.data(), name_nickname_.size());
#else
    ImGui::InputText(
        "Family name", name_family_.data(), name_family_.size());
    ImGui::InputText(
        "Given name", name_given_.data(), name_given_.size());
    ImGui::InputText(
        "Family reading", name_family_reading_.data(),
        name_family_reading_.size());
    ImGui::InputText(
        "Given reading", name_given_reading_.data(),
        name_given_reading_.size());
    ImGui::InputText(
        "Nickname", name_nickname_.data(), name_nickname_.size());
#endif
    if (!name_error_.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
            name_error_.c_str());
    }
#ifdef __ANDROID__
    if (ImGui::Button("Start Game", ImVec2(-FLT_MIN, 0.0f))) {
        if (name_family_[0] == '\0' || name_given_[0] == '\0'
            || name_family_reading_[0] == '\0'
            || name_given_reading_[0] == '\0'
            || name_nickname_[0] == '\0') {
            name_error_ = "Every field must contain a name.";
        } else {
            player_name_ = {
                name_family_.data(),
                name_given_.data(),
                name_family_reading_.data(),
                name_given_reading_.data(),
                name_nickname_.data(),
                name_nickname_.data(),
            };
            name_input_open_ = false;
            start_new_game();
        }
    }
    if (ImGui::Button("Reset Defaults", ImVec2(-FLT_MIN, 0.0f))) {
        open_name_input();
    }
    if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0.0f))) {
        name_input_open_ = false;
        title_started_ = std::chrono::steady_clock::now()
            - std::chrono::milliseconds(120 * 1000 / 60);
    }
#else
    if (ImGui::Button("Start Game", ImVec2(120.0f, 0.0f))) {
        if (name_family_[0] == '\0' || name_given_[0] == '\0'
            || name_family_reading_[0] == '\0'
            || name_given_reading_[0] == '\0'
            || name_nickname_[0] == '\0') {
            name_error_ = "Every field must contain a name.";
        } else {
            player_name_ = {
                name_family_.data(),
                name_given_.data(),
                name_family_reading_.data(),
                name_given_reading_.data(),
                name_nickname_.data(),
                name_nickname_.data(),
            };
            name_input_open_ = false;
            start_new_game();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults", ImVec2(130.0f, 0.0f))) {
        open_name_input();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
        name_input_open_ = false;
        title_started_ = std::chrono::steady_clock::now()
            - std::chrono::milliseconds(120 * 1000 / 60);
    }
#endif
#ifdef __ANDROID__
    imgui_->touch_drag_scroll();
    ImGui::EndChild();
#endif
    ImGui::End();
}


}  // namespace th2app
