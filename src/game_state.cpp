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

float Game::bgm_gain(int volume) const
{
    if (suppress_audio_output_ || config_.bgm_muted) {
        return 0.0f;
    }
    return std::clamp(volume, 0, 255) / 255.0f
        * config_.bgm_volume / 256.0f;
}

float Game::se_gain(int volume) const
{
    if (suppress_audio_output_ || config_.se_muted) {
        return 0.0f;
    }
    return std::clamp(volume, 0, 255) / 255.0f
        * config_.se_volume / 256.0f;
}

std::filesystem::path Game::anime4k_shader_dir() const
{
    const auto base = std::filesystem::path(SDL_GetBasePath());
    const auto executable_relative = base / TH2_ANIME4K_SHADER_DIR;
    if (std::filesystem::exists(executable_relative)) {
        return executable_relative;
    }
    return base / ".." / "Resources" / TH2_ANIME4K_SHADER_DIR;
}

void Game::ensure_upscaler()
{
    if (last_anime4k_wanted_ == config_.anime4k) {
        return;
    }
    last_anime4k_wanted_ = config_.anime4k;
    upscaler_ = th2::create_upscaler(
        renderer_, anime4k_shader_dir(), config_.anime4k,
        &anime4k_available_);
}

std::size_t Game::voice_character_index(int character) const
{
    if (character >= 1 && character <= 9) {
        return static_cast<std::size_t>(character - 1);
    }
    if (character == 28) {
        return 10;
    }
    if (character == 99) {
        return 9;
    }
    return 10;
}

float Game::voice_gain(int volume, int character) const
{
    const auto index = voice_character_index(character);
    if (suppress_audio_output_ || config_.voice_muted
        || config_.character_voice_muted[index]) {
        return 0.0f;
    }
    return std::clamp(volume, 0, 256) / 256.0f
        * config_.voice_volume / 256.0f
        * config_.character_voice_volume[index] / 256.0f;
}

void Game::apply_audio_gains()
{
    bgm_.set_gain(bgm_gain(bgm_volume_));
    for (std::size_t i = 0; i < transient_se_.size(); ++i) {
        transient_se_[i].set_gain(se_gain(transient_se_volume_[i]));
    }
    for (std::size_t i = 0; i < se_channels_.size(); ++i) {
        se_channels_[i].set_gain(se_gain(se_volume_[i]));
    }
    for (std::size_t i = 0; i < voice_channels_.size(); ++i) {
        voice_channels_[i].set_gain(
            voice_gain(voice_volume_[i], voice_character_[i]));
    }
}

void Game::sync_window_config()
{
#ifndef __ANDROID__
    if (!window_) {
        return;
    }
    config_.fullscreen =
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (config_.fullscreen) {
        return;
    }
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    if (width > 0 && height > 0) {
        config_.window_x = x;
        config_.window_y = y;
        config_.window_width = width;
        config_.window_height = height;
    }
#endif
}

void Game::toggle_fullscreen()
{
#ifndef __ANDROID__
    config_.fullscreen =
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0;
    SDL_SetWindowFullscreen(window_, config_.fullscreen);
    sync_window_config();
    th2::save_config(config_path_, config_);
#endif
}

void Game::start_movie(int mode, int number, bool resume_script)
{
    std::string name;
    SDL_FRect destination{0.0f, 0.0f, 800.0f, 600.0f};
    switch (mode) {
    case 0:
        name = "TH2_OP_800x448_5M.avi";
        destination = {0.0f, 76.0f, 800.0f, 448.0f};
        break;
    case 1:
        name = std::format("TH2_ED_{:02d}_800_3M.avi", number);
        break;
    case 2:
        name = "TH2_TR_800x600_5M.avi";
        break;
    case 3:
        name = "Leaf_800x600_5M.avi";
        break;
    default:
        throw std::runtime_error("unsupported movie mode");
    }
    const auto* entry = movie_archive_.find(name);
    if (!entry) {
        throw std::runtime_error("movie not found: " + name);
    }
    movie_bytes_ = movie_archive_.read(*entry);
    movie_ = std::make_unique<th2::VideoPlayer>(
        renderer_, movie_bytes_, destination);
    movie_resume_script_ = resume_script;
    movie_mode_ = mode;
    if (mode == 0) {
        play_bgm(0, false, 255);
    } else if (mode == 1) {
        play_bgm(50, false, 255);
    } else if (mode == 2) {
        play_bgm(99, false, 255);
    }
}

void Game::update_movie()
{
    if (!movie_) {
        return;
    }
    movie_->update();
    if (!movie_->finished()) {
        return;
    }
    complete_movie();
}

void Game::complete_movie()
{
    const int completed_mode = movie_mode_;
    movie_.reset();
    movie_bytes_.clear();
    movie_mode_ = -1;
    if (completed_mode != 3) {
        bgm_.stop();
        bgm_track_ = -1;
    }
    if (movie_resume_script_) {
        movie_resume_script_ = false;
        advance();
    } else if (completed_mode == 3 && runtime_.game_flag(98) != 0) {
        start_movie(0, 0, false);
    } else {
        title_started_ = std::chrono::steady_clock::now();
    }
}

th2::AudioChannel& Game::waited_audio_channel()
{
    if (audio_wait_->kind == AudioWaitKind::bgm) {
        return bgm_;
    }
    if (audio_wait_->kind == AudioWaitKind::voice) {
        return voice_channels_.at(audio_wait_->channel);
    }
    if (audio_wait_->channel < se_channels_.size()) {
        return se_channels_.at(audio_wait_->channel);
    }
    return transient_se_.at(audio_wait_->channel - se_channels_.size());
}


}  // namespace th2app
