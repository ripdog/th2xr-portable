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

void Game::begin_background_scroll(
    float x, float y, float width, float height, int frames, int type)
{
    background_scroll_ = BackgroundScroll{
        current_background_view(),
        {x, y, width, height},
        std::max(1, frames) * 2,
        type % 3,
        type / 3 == 1,
        std::chrono::steady_clock::now(),
    };
}

void Game::load_character_texture(const th2::CharacterState& character)
{
    auto& loaded = character_texture(character.number);
    if (loaded.pose != character.pose || !loaded.texture) {
        loaded.texture = load_toned_texture(
            renderer_, graphics_,
            th2::character_asset_name(character.number, character.pose),
            graphics_, character_tone_curves());
        loaded.pose = character.pose;
    }
}

void Game::reload_character_textures()
{
    character_textures_ = {};
    for (const auto& character : characters_.ordered()) {
        load_character_texture(character);
    }
}

void Game::apply_staged_characters()
{
    for (std::size_t index = 0; index < character_pending_removal_.size();
         ++index) {
        if (character_pending_removal_[index]) {
            characters_.remove(static_cast<int>(index));
            character_textures_[index] = {};
            character_animations_[index] = {};
        }
        character_pending_removal_[index] = false;
        character_staged_[index] = false;
    }
}

void Game::clear_characters()
{
    characters_.clear();
    character_textures_ = {};
    character_animations_ = {};
    character_staged_ = {};
    character_pending_removal_ = {};
}

std::size_t Game::character_index(int character_number) const
{
    if (character_number < 0
        || static_cast<std::size_t>(character_number)
            >= character_textures_.size()) {
        throw std::out_of_range(std::format(
            "invalid character number: {}", character_number));
    }
    return static_cast<std::size_t>(character_number);
}

std::vector<ToneCurveSpec> Game::effect_tone_curves(
    int tone, bool character)
{
    switch (tone / 4) {
    case 1: return {{"sepia.amp", 0}};
    case 2: return {{"nega.amp", 256}};
    case 3: return {{"", 0}};
    case 4: return {{"blue.amp", 128}};
    case 5: return {{"red.amp", 128}};
    case 6: return {{"green.amp", 128}};
    case 7: return {{"blue2.amp", 128}};
    case 8: return {{"brown.amp", 128}};
    case 9: return {{"sepia_half.amp", 128}};
    case 10: return {{"black.amp", character ? 0 : 256}};
    case 11: return {{"yoritomo.amp", character ? 0 : 256}};
    default: return {};
    }
}

std::string Game::base_tone_curve(int tone)
{
    switch (tone % 4) {
    case 1: return "evening.amp";
    case 2: return "night.amp";
    case 3: return "indoor.amp";
    default: return {};
    }
}

std::vector<ToneCurveSpec> Game::background_tone_curves() const
{
    const int tone = tone_back_ < 0 ? tone_ : tone_back_;
    return effect_tone_curves(tone, false);
}

std::vector<ToneCurveSpec> Game::character_tone_curves() const
{
    const int tone = tone_char_ < 0 ? tone_ : tone_char_;
    std::vector<ToneCurveSpec> result;
    if (tone_char_ < 0 && !background_tone_curve_.empty()) {
        result.push_back({background_tone_curve_, 256});
    } else if (const auto base = base_tone_curve(tone); !base.empty()) {
        result.push_back({base, 256});
    }
    if (weather_ != 0) {
        result.push_back({"rain.amp", 256});
    }
    auto effect = effect_tone_curves(tone, true);
    result.insert(result.end(), effect.begin(), effect.end());
    return result;
}

int Game::character_effect_frames(int frames)
{
    return frames < 0 ? 15 : std::max(frames, 0);
}

bool Game::character_animation_active() const
{
    return std::ranges::any_of(
        character_animations_, [](const CharacterAnimation& animation) {
            return animation.kind != CharacterAnimationKind::none;
        });
}

void Game::start_character_animation(
    int character_number, CharacterAnimation animation)
{
    const auto index = character_index(character_number);
    animation.frames = character_effect_frames(animation.frames);
    animation.started = std::chrono::steady_clock::now();
    if (animation.frames == 0) {
        if (animation.kind == CharacterAnimationKind::leave) {
            characters_.remove(character_number);
            character_textures_[index] = {};
            character_staged_[index] = false;
            character_pending_removal_[index] = false;
        }
        return;
    }
    character_animations_[index] = std::move(animation);
}

void Game::update_character_animations()
{
    bool resume = false;
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < character_animations_.size(); ++index) {
        auto& animation = character_animations_[index];
        if (animation.kind == CharacterAnimationKind::none) {
            continue;
        }
        const auto elapsed = std::chrono::duration<double>(
            now - animation.started).count() * 60.0;
        if (elapsed < animation.frames) {
            continue;
        }
        if (animation.kind == CharacterAnimationKind::leave) {
            characters_.remove(static_cast<int>(index));
            character_textures_[index] = {};
            character_staged_[index] = false;
            character_pending_removal_[index] = false;
        }
        resume |= animation.blocking;
        animation = {};
    }
    if (resume) {
        advance();
    }
}

void Game::set_character(const th2::Event& event)
{
    const int character_number = number(event, 0);
    const auto index = character_index(character_number);
    const auto* previous = characters_.find(character_number);
    int locate = number(event, 2);
    if (locate < 0) {
        locate = event.instruction.name != "SetChar" && previous
            ? previous->locate : 1;
    }
    const bool wait_form = event.instruction.name == "CW";
    const std::size_t layer_index = wait_form ? 3 : 4;
    const std::size_t brightness_index = wait_form ? 4 : 5;
    const std::size_t alpha_index = wait_form ? 5 : 6;
    const int layer = number(event, layer_index) < 0
        ? 0 : number(event, layer_index);
    const int brightness = number(event, brightness_index) < 0
        ? 128 : number(event, brightness_index);
    const int alpha = number(event, alpha_index) < 0
        ? 256 : number(event, alpha_index);
    if (previous && previous->pose == number(event, 1)
        && previous->locate == locate && previous->layer == layer
        && previous->brightness == brightness
        && previous->alpha == alpha) {
        return;
    }
    const int animation_type = wait_form ? 3
        : number(event, 3) == -2 ? -1
        : number(event, 3) < 0 ? 0 : number(event, 3);
    if (animation_type != 3) {
        message_visible_ = false;
    }
    CharacterAnimation animation;
    animation.from_locate = previous ? previous->locate : locate;
    animation.to_locate = locate;
    animation.from_brightness = previous ? previous->brightness : brightness;
    animation.to_brightness = brightness;
    animation.from_alpha = previous ? previous->alpha : alpha;
    animation.to_alpha = alpha;
    animation.blocking = event.instruction.name == "C";
    bool stage = wait_form;
    if (event.instruction.name == "C") {
        animation.type = animation_type;
        stage = animation.type == 3;
        animation.frames = animation.type == -1 ? 0 : number(event, 7);
        animation.kind = previous && previous->pose != number(event, 1)
            ? CharacterAnimationKind::pose
            : CharacterAnimationKind::enter;
    }
    if (animation.kind == CharacterAnimationKind::pose && previous) {
        auto& loaded = character_textures_[index];
        animation.previous = std::move(loaded.texture);
        loaded.pose = -1;
    }
    auto& character = characters_.set(
        character_number, number(event, 1), locate, layer,
        brightness, alpha);
    load_character_texture(character);
    character_pending_removal_[index] = false;
    character_staged_[index] = stage;
    if (!stage && event.instruction.name != "SetChar") {
        start_character_animation(character_number, std::move(animation));
    }
}

void Game::play_se(int channel, int sound, bool loop, int volume, int fade,
             bool wait_for_completion)
{
    const auto name = std::format("SE_{:04d}.WAV", sound);
    if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
        se_channels_[channel].play(
            load_audio(se_archive_, name), loop,
            fade > 0 ? 0.0f : se_gain(volume));
        if (fade > 0) {
            se_channels_[channel].fade_to(
                se_gain(volume),
                std::chrono::milliseconds(fade * 1000 / 60));
        }
        se_sound_[channel] = sound;
        se_loop_[channel] = loop;
        se_volume_[channel] = volume;
        return;
    }
    auto found = std::find_if(
        transient_se_.begin(), transient_se_.end(),
        [](const th2::AudioChannel& audio) { return !audio.playing(); });
    if (found == transient_se_.end()) {
        found = transient_se_.begin();
    }
    const auto index = static_cast<std::size_t>(
        std::distance(transient_se_.begin(), found));
    transient_se_volume_[index] = volume;
    found->play(load_audio(se_archive_, name), false, se_gain(volume));
    if (wait_for_completion) {
        audio_wait_ = AudioWait{
            AudioWaitKind::sound_effect, se_channels_.size() + index};
    }
}

void Game::sync_game_flags()
{
    const auto flags = runtime_.all_game_flags();
    if (std::ranges::equal(flags, persistent_game_flags_)) {
        return;
    }
    std::ranges::copy(flags, persistent_game_flags_.begin());
    persistent_state_.save_game_flags(persistent_game_flags_);
}

void Game::play_bgm(int music, bool loop, int volume)
{
    static constexpr std::array music_room_tracks{
        0, 10, 29, 11, 12, 13, 14, 30, 27, 1,
        2, 4, 3, 5, 6, 8, 7, 9, 18, 37,
        38, 41, 42, 39, 40, 15, 16, 17, 19, 20,
        22, 32, 21, 23, 26, 31, 25, 24, 28, 50,
    };
    const auto music_slot =
        std::ranges::find(music_room_tracks, music);
    if (music_slot != music_room_tracks.end()) {
        runtime_.set_game_flag(
            128 + static_cast<std::size_t>(
                std::distance(music_room_tracks.begin(), music_slot)),
            1);
        sync_game_flags();
    }
    if (bgm_track_ == music) {
        return;
    }
    bgm_track_ = music;
    bgm_loop_ = loop;
    bgm_volume_ = volume;
    const auto gain = bgm_gain(volume);
    const auto single = std::format("BGM_{:03d}.OGG", music);
    if (bgm_archive_.find(single)) {
        bgm_.play(load_audio(bgm_archive_, single), loop, gain);
        return;
    }
    const auto intro = std::format("BGM_{:03d}_A.OGG", music);
    const auto body = std::format("BGM_{:03d}_B.OGG", music);
    if (!bgm_archive_.find(intro) || !bgm_archive_.find(body)) {
        throw std::runtime_error("BGM track not found: " + std::to_string(music));
    }
    if (loop) {
        bgm_.play_intro_loop(
            load_audio(bgm_archive_, intro), load_audio(bgm_archive_, body), gain);
    } else {
        bgm_.play(load_audio(bgm_archive_, intro), false, gain);
    }
}

void Game::play_voice(const th2::Event& event)
{
    int character = number(event, 0);
    if (character >= 10 && character != 28) {
        character = 99;
    }
    const int volume = number(event, 1) < 0
        ? (event.instruction.name == "VV" ? 256 : 255)
        : number(event, 1);
    const bool loop = number(event, 2) > 0;
    const int voice = number(event, 3);
    const int channel = number(event, 4) < 0 ? 0 : number(event, 4);
    if (channel < 0
        || static_cast<std::size_t>(channel) >= voice_channels_.size()) {
        return;
    }
    int scenario = scenario_number(runtime_.script_name());
    if (vi_event_voice_no_all_ >= 0) {
        scenario = vi_event_voice_no_all_;
    } else if (vi_event_voice_no_ >= 0) {
        scenario = scenario / 100 * 100 + vi_event_voice_no_;
    }
    const auto standard_name = std::format(
        "K{:09d}_{:03d}{:03d}.OGG", scenario, voice, character);
    auto name = standard_name;
    auto& voice_channel = voice_channels_[channel];
    voice_channel.stop();
    if (runtime_.flag(5) == 0) {
        const auto alternate_name = std::format(
            "K{:09d}_{:03d}{:03d}A.OGG",
            scenario, voice, character);
        if (voice_archive_.find(alternate_name)) {
            name = alternate_name;
        } else if (event.instruction.name == "VC") {
            voice_sound_[channel] = -1;
            voice_loop_[channel] = false;
            return;
        }
    }
    const bool alternate = name != standard_name;
    pending_backlog_voice_ = BacklogVoice{
        0, 0, scenario, voice, character, volume, alternate};
    const auto* voice_entry = voice_archive_.find(name);
    if (!voice_entry) {
        voice_sound_[channel] = -1;
        voice_loop_[channel] = false;
        return;
    }
    voice_channel.play(
        th2::decode_audio(voice_archive_.read(*voice_entry)), loop,
        voice_gain(volume, character));
    voice_sound_[channel] = voice;
    voice_character_[channel] = character;
    voice_scenario_[channel] = scenario;
    voice_volume_[channel] = volume;
    voice_loop_[channel] = loop;
}

void Game::replay_backlog_voice(const Game::BacklogVoice& voice)
{
    auto name = std::format(
        "K{:09d}_{:03d}{:03d}{}.OGG",
        voice.scenario, voice.voice, voice.character,
        voice.alternate ? "A" : "");
    if (!voice_archive_.find(name) && voice.alternate) {
        name = std::format(
            "K{:09d}_{:03d}{:03d}.OGG",
            voice.scenario, voice.voice, voice.character);
    }
    if (!voice_archive_.find(name)) {
        return;
    }
    voice_channels_[0].stop();
    voice_channels_[0].play(
        load_audio(voice_archive_, name), false,
        voice_gain(voice.volume, voice.character));
    voice_sound_[0] = voice.voice;
    voice_character_[0] = voice.character;
    voice_scenario_[0] = voice.scenario;
    voice_volume_[0] = voice.volume;
    voice_loop_[0] = false;
}

void Game::update_audio()
{
    bgm_.update();
    for (auto& channel : transient_se_) {
        channel.update();
    }
    for (auto& channel : se_channels_) {
        channel.update();
    }
    for (auto& channel : voice_channels_) {
        channel.update();
    }
    if (audio_wait_) {
        const auto& channel = waited_audio_channel();
        const bool complete = audio_wait_->kind == AudioWaitKind::bgm
            ? !channel.fading()
            : !channel.playing();
        if (complete) {
            audio_wait_.reset();
            advance();
        }
    }
}

void Game::set_background(const th2::Event& event, bool keep_characters)
{
    if (number(event, 1) < 0) {
        background_.reset();
        bg_scene_ = -1;
        background_kind_ = BackgroundKind::background;
        background_tone_curve_.clear();
        background_view_ = {0.0f, 0.0f, 800.0f, 600.0f};
        background_scroll_.reset();
        return;
    }
    int scene = number(event, 1) * 10
        + std::max<std::int32_t>(0, number(event, 2));
    scene = seasonal_background_scene(scene);
    bg_scene_ = scene;
    background_kind_ = BackgroundKind::background;
    background_view_ = {
        static_cast<float>(std::max(0, number(event, 4))),
        static_cast<float>(std::max(0, number(event, 5))),
        800.0f,
        600.0f,
    };
    background_scroll_.reset();
    update_background_sakura(scene, true);
    if (keep_characters) {
        apply_staged_characters();
    } else {
        clear_characters();
    }
    const auto name = std::format(
        "B{:03d}{}{}{}.bmp", scene / 10,
        (tone_back_ < 0 ? tone_ : tone_back_) % 4,
        weather_, scene % 10);
    const auto curve_name =
        std::filesystem::path(name).replace_extension(".amp").string();
    background_tone_curve_ =
        graphics_.find(curve_name) ? curve_name : std::string{};
    background_ = load_toned_texture(
        renderer_, backgrounds_, name, graphics_,
        background_tone_curves());
    if (keep_characters) {
        reload_character_textures();
    }
}

void Game::set_cg(
    const th2::Event& event, BackgroundKind kind, char prefix)
{
    int visual = number(event, 1) * 10;
    if (number(event, 2) >= 0) {
        visual += number(event, 2);
    }
    bg_scene_ = visual;
    background_kind_ = kind;
    background_view_ = {
        static_cast<float>(std::max(0, number(event, 5))),
        static_cast<float>(std::max(0, number(event, 6))),
        800.0f,
        600.0f,
    };
    background_scroll_.reset();
    update_background_sakura(visual, false);
    background_ = load_toned_texture(
        renderer_, graphics_, std::format("{}{:06d}.tga", prefix, visual),
        graphics_, background_tone_curves());
    auto& unlocked = kind == BackgroundKind::visual
        ? unlocked_visual_cgs_ : unlocked_h_cgs_;
    if (unlocked.emplace(visual).second) {
        persistent_state_.unlock(
            kind == BackgroundKind::visual
                ? th2::PersistentState::UnlockKind::visual_cg
                : th2::PersistentState::UnlockKind::h_cg,
            visual);
    }
    const bool keep_characters = number(event, 4) > 0;
    if (keep_characters) {
        apply_staged_characters();
        reload_character_textures();
    } else {
        clear_characters();
    }
}

void Game::restore_background()
{
    if (bg_scene_ < 0) {
        return;
    }
    if (background_kind_ != BackgroundKind::background) {
        const char prefix =
            background_kind_ == BackgroundKind::visual ? 'v' : 'h';
        background_ = load_toned_texture(
            renderer_, graphics_,
            std::format("{}{:06d}.tga", prefix, bg_scene_),
            graphics_, background_tone_curves());
    } else {
        const auto name = std::format(
            "B{:03d}{}{}{}.bmp", bg_scene_ / 10,
            (tone_back_ < 0 ? tone_ : tone_back_) % 4,
            weather_, bg_scene_ % 10);
        const auto curve_name =
            std::filesystem::path(name).replace_extension(".amp").string();
        background_tone_curve_ =
            graphics_.find(curve_name) ? curve_name : std::string{};
        background_ = load_toned_texture(
            renderer_, backgrounds_, name, graphics_,
            background_tone_curves());
    }
    reload_character_textures();
}

std::optional<std::size_t> Game::overlay_index(int requested) const
{
    if (requested == -1) {
        return overlays_.size() - 1;
    }
    if (requested < 0
        || static_cast<std::size_t>(requested) >= overlays_.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(requested);
}

void Game::load_overlay(
    std::size_t slot, std::string name, std::string archive,
    int tone_type)
{
    const auto& source = archive == "bak" ? backgrounds_ : graphics_;
    overlays_[slot] = load_toned_texture(
        renderer_, source, name, graphics_,
        tone_type == 1
            ? character_tone_curves()
            : background_tone_curves(),
        &overlay_pixels_[slot]);
    auto& state = overlay_states_[slot];
    state = {};
    state.name = std::move(name);
    state.archive = std::move(archive);
    state.tone_type = tone_type;
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(overlays_[slot].get(), &width, &height);
    state.destination_width = static_cast<int>(width * 640.0f / 800.0f);
    state.destination_height = static_cast<int>(height * 448.0f / 600.0f);
    state.source_width = state.destination_width;
    state.source_height = state.destination_height;
}

void Game::apply_overlay_brightness(std::size_t slot)
{
    const auto& source = overlay_pixels_[slot];
    if (!source) {
        return;
    }
    Surface adjusted(
        SDL_ConvertSurface(source.get(), SDL_PIXELFORMAT_RGBA32));
    if (!adjusted) {
        throw std::runtime_error(SDL_GetError());
    }
    const auto& state = overlay_states_[slot];
    const std::array brightness{
        state.red, state.green, state.blue};
    auto* pixels = static_cast<std::uint8_t*>(adjusted->pixels);
    for (int y = 0; y < adjusted->h; ++y) {
        auto* row = pixels + static_cast<std::size_t>(y) * adjusted->pitch;
        for (int x = 0; x < adjusted->w; ++x) {
            auto* pixel = row + static_cast<std::size_t>(x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                const int value = pixel[channel];
                const int light = brightness[channel];
                pixel[channel] = static_cast<std::uint8_t>(
                    light < 128
                        ? value * light / 128
                        : value
                            + (255 - value) * (light - 128) / 128);
            }
        }
    }
    overlays_[slot] = texture_from_surface(adjusted.get());
}


}  // namespace th2app
