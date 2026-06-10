#include "archive.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "font.hpp"
#include "image.hpp"
#include "message.hpp"
#include "script_runtime.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

std::int32_t number(const th2::Event& event, std::size_t index)
{
    return std::get<std::int32_t>(event.arguments.at(index));
}

const std::string& text(const th2::Event& event, std::size_t index)
{
    return std::get<std::string>(event.arguments.at(index));
}

Texture load_texture(SDL_Renderer* renderer, const th2::Archive& archive,
                     std::string_view name)
{
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("image not found: " + std::string(name));
    }
    SDL_Surface* surface = th2::load_image(archive.read(*entry), entry->name);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (!texture) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return Texture(texture);
}

th2::AudioClip load_audio(const th2::Archive& archive, std::string_view name)
{
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("audio not found: " + std::string(name));
    }
    return th2::decode_audio(archive.read(*entry));
}

int scenario_number(std::string_view name)
{
    int result = 0;
    for (const auto byte : name) {
        if (byte >= '0' && byte <= '9') {
            result = result * 10 + byte - '0';
        } else if (result != 0) {
            break;
        }
    }
    return result;
}

std::vector<std::string> display_lines(std::string_view source)
{
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t position = 0; position < source.size();) {
        if (source[position] == '\n') {
            lines.push_back(line);
            line.clear();
            ++position;
            continue;
        }
        line.push_back(source[position++]);
        if (line.size() >= 58) {
            const auto space = line.find_last_of(' ');
            if (space != std::string::npos && space > 35) {
                lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
            } else {
                lines.push_back(line);
                line.clear();
            }
        }
    }
    lines.push_back(line);
    return lines;
}

class Game {
public:
    explicit Game(const std::filesystem::path& data)
        : scripts_(data / "SDT.PAK"), graphics_(data / "GRP.PAK"),
          backgrounds_(data / "bak.pak"), fonts_(data / "FNT.PAK"),
          bgm_archive_(data / "bgm.PAK"), se_archive_(data / "SE.PAK"),
          voice_archive_(data / "voice.pak"), runtime_(scripts_), font_(fonts_)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            throw std::runtime_error(SDL_GetError());
        }
        if (!SDL_CreateWindowAndRenderer(
                "ToHeart2 XRATED", 800, 600, SDL_WINDOW_RESIZABLE,
                &window_, &renderer_)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetRenderLogicalPresentation(
            renderer_, 800, 600, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        runtime_.load("EV_0301MORNING.SDT");

        auto try_load = [&](std::string_view name) -> Texture {
            const auto* entry = graphics_.find(name);
            if (!entry) return {};
            try {
                return load_texture(renderer_, graphics_, name);
            } catch (...) {
                return {};
            }
        };
        ui_sys_menu_bg_ = try_load("sys0100.tga");
        ui_sys_menu_btns_ = try_load("sys0110.tga");
        ui_sys_cancel_ = try_load("sys0111.tga");
        ui_sidebar_track_ = try_load("sys0000.tga");
        ui_sidebar_btns_ = try_load("sys0001.tga");
        ui_keywait_ = try_load("sys0011.tga");
        ui_pageend_ = try_load("sys0010.tga");
    }

    ~Game()
    {
        for (auto& overlay : overlays_) {
            overlay.reset();
        }
        background_.reset();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    int run()
    {
        advance();
        while (running_) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running_ = false;
                    continue;
                }
                SDL_ConvertEventToRenderCoordinates(renderer_, &event);

                // UI mode routing
                if (ui_mode_ == UiMode::system_menu) {
                    handle_system_menu_input(event);
                    continue;
                }
                if (ui_mode_ == UiMode::backlog) {
                    handle_backlog_input(event);
                    continue;
                }

                // Message window hidden - any input restores it
                if (!message_visible_) {
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                        || event.type == SDL_EVENT_KEY_DOWN) {
                        message_visible_ = true;
                    }
                    continue;
                }

                if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.key == SDLK_PAGEUP) {
                        open_backlog();
                    } else if (choosing_) {
                        if (event.key.key == SDLK_RETURN
                            || event.key.key == SDLK_SPACE) {
                            choice_selected_ = choice_highlight_;
                            advance();
                        } else if (event.key.key == SDLK_UP) {
                            if (choice_highlight_ > 0) {
                                --choice_highlight_;
                            }
                        } else if (event.key.key == SDLK_DOWN) {
                            if (choice_highlight_ + 1
                                < static_cast<int>(choices_.size())) {
                                ++choice_highlight_;
                            }
                        }
                    } else {
                        if (event.key.key == SDLK_ESCAPE) {
                            open_system_menu();
                        } else if (event.key.key == SDLK_F5) {
                            save(0);
                        } else if (event.key.key == SDLK_F7) {
                            load(0);
                        } else if (event.key.key == SDLK_F11) {
                            SDL_SetWindowFullscreen(
                                window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN));
                        } else if (event.key.key == SDLK_RETURN
                                   || event.key.key == SDLK_SPACE) {
                            advance();
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    if (event.button.button == SDL_BUTTON_RIGHT) {
                        open_system_menu();
                    } else if (event.button.button == SDL_BUTTON_LEFT) {
                        if (handle_sidebar_click(event.button.x, event.button.y)) {
                            continue;
                        } else if (choosing_) {
                            const float mouse_y = event.button.y;
                            float y = choice_y_start();
                            for (int i = 0;
                                 i < static_cast<int>(choices_.size()); ++i) {
                                if (mouse_y >= y && mouse_y < y + 31.0f) {
                                    choice_selected_ = i;
                                    advance();
                                    break;
                                }
                                y += 31.0f;
                            }
                        } else {
                            advance();
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (event.wheel.y > 0) {
                        open_backlog();
                    } else if (event.wheel.y < 0) {
                        advance();
                    }
                } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    update_sidebar_hover(event.motion.x, event.motion.y);
                    if (choosing_) {
                        const float mouse_y = event.motion.y;
                        float y = choice_y_start();
                        for (int i = 0;
                             i < static_cast<int>(choices_.size()); ++i) {
                            if (mouse_y >= y && mouse_y < y + 31.0f) {
                                choice_highlight_ = i;
                                break;
                            }
                            y += 31.0f;
                        }
                    }
                }
            }
            const bool control_held =
                (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            if (control_held) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= skip_next_time_) {
                    skip();
                    skip_next_time_ = now + std::chrono::milliseconds(40);
                }
            } else if (wake_time_
                       && std::chrono::steady_clock::now() >= *wake_time_) {
                wake_time_.reset();
                advance();
            }
            update_audio();
            draw();
            SDL_Delay(8);
        }
        return 0;
    }

private:
    enum class AudioWaitKind {
        sound_effect,
        voice,
    };

    struct AudioWait {
        AudioWaitKind kind;
        std::size_t channel;
    };

    struct CharacterTexture {
        int pose = -1;
        Texture texture;
    };

    th2::Archive scripts_;
    th2::Archive graphics_;
    th2::Archive backgrounds_;
    th2::Archive fonts_;
    th2::Archive bgm_archive_;
    th2::Archive se_archive_;
    th2::Archive voice_archive_;
    th2::ScriptRuntime runtime_;
    th2::GameFont font_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    Texture background_;
    int bg_scene_ = -1;
    bool bg_is_visual_ = false;
    std::array<Texture, 32> overlays_{};
    th2::Characters characters_;
    std::array<CharacterTexture, 32> character_textures_{};
    th2::AudioChannel bgm_;
    int bgm_track_ = -1;
    bool bgm_loop_ = false;
    int bgm_volume_ = 255;
    std::array<th2::AudioChannel, 8> transient_se_{};
    std::array<th2::AudioChannel, 16> se_channels_{};
    std::array<int, 16> se_sound_{};  // sound number per channel, -1 = none
    std::array<bool, 16> se_loop_{};
    std::array<int, 16> se_volume_{};
    std::array<th2::AudioChannel, 8> voice_channels_{};
    std::array<int, 8> voice_sound_{};  // voice number per channel, -1 = none
    std::array<int, 8> voice_character_{};  // character for this voice
    std::array<int, 8> voice_scenario_{};  // scenario for voice filename
    std::array<int, 8> voice_volume_{};
    std::array<bool, 8> voice_loop_{};

    // UI textures (from GRP.PAK)
    Texture ui_sys_menu_bg_;       // sys0100.tga
    Texture ui_sys_menu_btns_;     // sys0110.tga
    Texture ui_sys_cancel_;        // sys0111.tga
    Texture ui_sidebar_track_;     // sys0000.tga
    Texture ui_sidebar_btns_;      // sys0001.tga
    Texture ui_keywait_;           // sys0011.tga (mid-page cursor)
    Texture ui_pageend_;           // sys0010.tga (end-of-page cursor)
    th2::Message message_;
    int tone_ = 0;
    int weather_ = 0;
    bool running_ = true;
    bool waiting_for_input_ = false;
    std::optional<std::chrono::steady_clock::time_point> wake_time_;
    std::optional<AudioWait> audio_wait_;
    std::chrono::steady_clock::time_point skip_next_time_{};

    struct Choice {
        std::string text;
        int flag_no = -1;
        int flag_value = 0;
        std::string sno;
    };
    bool choosing_ = false;
    std::vector<Choice> choices_;
    int choice_highlight_ = 0;
    int choice_selected_ = -1;
    int choice_result_register_ = -1;
    bool choice_ex_ = false;

    // --- UI State ---
    enum class UiMode { game, system_menu, backlog };
    UiMode ui_mode_ = UiMode::game;
    int menu_highlight_ = 0;
    struct BacklogEntry { std::string text; };
    std::vector<BacklogEntry> backlog_;
    int backlog_depth_ = 0;
    int sidebar_hover_ = -1;
    bool message_visible_ = true;

    float choice_y_start() const
    {
        if (!message_.empty()) {
            return 104.0f;
        }
        return 468.0f;
    }

    void skip()
    {
        if (choosing_) {
            return;
        }
        wake_time_.reset();
        if (audio_wait_) {
            auto& channel = audio_wait_->kind == AudioWaitKind::sound_effect
                ? se_channels_.at(audio_wait_->channel)
                : voice_channels_.at(audio_wait_->channel);
            channel.stop();
            audio_wait_.reset();
        }
        if (waiting_for_input_) {
            if (message_.reveal_next()) {
                return;
            }
            waiting_for_input_ = false;
        }
        advance(true);
    }

    CharacterTexture& character_texture(int number)
    {
        if (number < 0
            || static_cast<std::size_t>(number) >= character_textures_.size()) {
            throw std::out_of_range("unsupported character number");
        }
        return character_textures_[number];
    }

    void load_character_texture(const th2::CharacterState& character)
    {
        auto& loaded = character_texture(character.number);
        if (loaded.pose != character.pose || !loaded.texture) {
            loaded.texture = load_texture(
                renderer_, graphics_,
                th2::character_asset_name(character.number, character.pose));
            loaded.pose = character.pose;
        }
    }

    void set_character(const th2::Event& event)
    {
        const int character_number = number(event, 0);
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
        auto& character = characters_.set(
            character_number, number(event, 1), locate, layer,
            brightness, alpha);
        load_character_texture(character);
    }

    void play_se(int channel, int sound, bool loop, int volume)
    {
        const auto name = std::format("SE_{:04d}.WAV", sound);
        if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
            se_channels_[channel].play(
                load_audio(se_archive_, name), loop,
                std::clamp(volume, 0, 255) / 255.0f);
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
        found->play(
            load_audio(se_archive_, name), false,
            std::clamp(volume, 0, 255) / 255.0f);
    }

    void play_bgm(int music, bool loop, int volume)
    {
        bgm_track_ = music;
        bgm_loop_ = loop;
        bgm_volume_ = volume;
        const auto gain = std::clamp(volume, 0, 255) / 255.0f;
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

    void play_voice(const th2::Event& event)
    {
        int character = number(event, 0);
        if (character >= 10 && character != 28) {
            character = 99;
        }
        const int volume = number(event, 1) < 0 ? 255 : number(event, 1);
        const bool loop = number(event, 2) > 0;
        const int voice = number(event, 3);
        const int channel = number(event, 4) < 0 ? 0 : number(event, 4);
        const int scenario = scenario_number(runtime_.script_name());
        const auto name = std::format(
            "K{:09d}_{:03d}{:03d}.OGG", scenario, voice, character);
        if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()) {
            voice_channels_[channel].play(
                load_audio(voice_archive_, name), loop,
                std::clamp(volume, 0, 256) / 256.0f);
            voice_sound_[channel] = voice;
            voice_character_[channel] = character;
            voice_scenario_[channel] = scenario;
            voice_volume_[channel] = volume;
            voice_loop_[channel] = loop;
        }
    }

    void update_audio()
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
            const auto& channel = audio_wait_->kind == AudioWaitKind::sound_effect
                ? se_channels_.at(audio_wait_->channel)
                : voice_channels_.at(audio_wait_->channel);
            if (!channel.playing()) {
                audio_wait_.reset();
                advance();
            }
        }
    }

    void set_background(const th2::Event& event)
    {
        const int scene = number(event, 1) * 10
            + std::max<std::int32_t>(0, number(event, 2));
        bg_scene_ = scene;
        bg_is_visual_ = false;
        const auto name = std::format(
            "B{:03d}{}{}{}.bmp", scene / 10, tone_ % 4, weather_, scene % 10);
        background_ = load_texture(renderer_, backgrounds_, name);
    }

    void set_visual(const th2::Event& event)
    {
        int visual = number(event, 1) * 10;
        if (number(event, 2) >= 0) {
            visual += number(event, 2);
        }
        bg_scene_ = visual;
        bg_is_visual_ = true;
        background_ = load_texture(
            renderer_, graphics_, std::format("v{:06d}.tga", visual));
    }

    void restore_background()
    {
        if (bg_scene_ < 0) {
            return;
        }
        if (bg_is_visual_) {
            background_ = load_texture(
                renderer_, graphics_,
                std::format("v{:06d}.tga", bg_scene_));
        } else {
            const auto name = std::format(
                "B{:03d}{}{}{}.bmp", bg_scene_ / 10, tone_ % 4,
                weather_, bg_scene_ % 10);
            background_ = load_texture(renderer_, backgrounds_, name);
        }
    }

    void handle(const th2::Event& event)
    {
        const auto name = event.instruction.name;
        if (name == "B" || name == "BT" || name == "BC" || name == "BCT") {
            if (number(event, 1) >= 0) {
                set_background(event);
            }
        } else if (name == "V" || name == "VT") {
            set_visual(event);
        } else if (name == "BD") {
            background_.reset();
            bg_scene_ = -1;
        } else if (name == "SetTimeMode") {
            tone_ = std::max<std::int32_t>(0, number(event, 0));
        } else if (name == "SetWeatherMode") {
            weather_ = std::max<std::int32_t>(0, number(event, 0));
        } else if (name == "SetBmpEx") {
            const auto slot = static_cast<std::size_t>(number(event, 0));
            if (slot < overlays_.size()) {
                const auto& archive = text(event, 6) == "bak" ? backgrounds_ : graphics_;
                overlays_[slot] = load_texture(renderer_, archive, text(event, 2));
            }
        } else if (name == "ResetBmp") {
            const auto slot = static_cast<std::size_t>(number(event, 0));
            if (slot < overlays_.size()) {
                overlays_[slot].reset();
            }
        } else if (name == "ResetBmpAll") {
            for (auto& overlay : overlays_) {
                overlay.reset();
            }
        } else if (name == "C" || name == "CW" || name == "SetChar") {
            set_character(event);
        } else if (name == "CR" || name == "CRW" || name == "ResetChar") {
            const int character_number = number(event, 0);
            characters_.remove(character_number);
            if (character_number >= 0
                && static_cast<std::size_t>(character_number)
                    < character_textures_.size()) {
                character_textures_[character_number] = {};
            }
        } else if (name == "CP" || name == "SetCharPose") {
            const int character_number = number(event, 0);
            if (auto* character = characters_.find(character_number)) {
                character->pose = number(event, 1);
                load_character_texture(*character);
            }
        } else if (name == "CL" || name == "SetCharLocate") {
            if (auto* character = characters_.find(number(event, 0))) {
                character->locate = number(event, 1);
            }
        } else if (name == "CY" || name == "SetCharLayer") {
            if (auto* character = characters_.find(number(event, 0))) {
                character->layer = number(event, 1);
            }
        } else if (name == "CB") {
            if (auto* character = characters_.find(number(event, 0))) {
                character->brightness = number(event, 1);
            }
        } else if (name == "CA") {
            if (auto* character = characters_.find(number(event, 0))) {
                character->alpha = number(event, 1);
            }
        } else if (name == "SetMessage2") {
            push_backlog();
            message_.set(text(event, 0));
            waiting_for_input_ = true;
        } else if (name == "AddMessage2") {
            message_.append(text(event, 0));
            waiting_for_input_ = true;
        } else if (name == "M") {
            const int music = number(event, 0);
            if (music < 0) {
                bgm_.stop();
                bgm_track_ = -1;
            } else {
                const int loop = number(event, 2) < 0 ? 1 : number(event, 2);
                const int volume = number(event, 3) < 0 ? 255 : number(event, 3);
                play_bgm(music, loop != 0, volume);
            }
        } else if (name == "MS") {
            bgm_.stop();
            bgm_track_ = -1;
        } else if (name == "MP") {
            bgm_.pause(number(event, 0) != 0);
        } else if (name == "MV") {
            bgm_.set_gain(std::clamp(number(event, 0), 0, 255) / 255.0f);
        } else if (name == "SE") {
            play_se(-1, number(event, 0), false,
                    number(event, 1) < 0 ? 255 : number(event, 1));
        } else if (name == "SEP") {
            play_se(
                number(event, 0), number(event, 1), number(event, 3) != 0,
                number(event, 4) < 0 ? 255 : number(event, 4));
        } else if (name == "SES") {
            const auto channel = number(event, 0);
            if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
                se_channels_[channel].stop();
                se_sound_[channel] = -1;
            }
        } else if (name == "SEV") {
            const auto channel = number(event, 0);
            if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
                se_channels_[channel].set_gain(
                    std::clamp(number(event, 1), 0, 255) / 255.0f);
            }
        } else if (name == "SEW" || name == "SEVW") {
            const auto channel = number(event, 0);
            if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()
                && se_channels_[channel].playing()) {
                audio_wait_ = AudioWait{
                    AudioWaitKind::sound_effect, static_cast<std::size_t>(channel)};
            }
        } else if (name == "VV" || name == "VA" || name == "VB"
                   || name == "VC") {
            play_voice(event);
        } else if (name == "VS") {
            const auto channel = number(event, 1) < 0 ? 0 : number(event, 1);
            if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()) {
                voice_channels_[channel].stop();
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
                text(event, 0),
                number(event, 1),
                number(event, 2),
            });
        } else if (name == "SetSelectMesEx") {
            choices_.push_back(Choice{
                text(event, 0),
                number(event, 2),
                number(event, 3),
                text(event, 1),
            });
            choice_ex_ = true;
        } else if (name == "SetSelect") {
            choice_result_register_ =
                std::get<th2::RegisterTarget>(event.arguments.at(0)).index;
            choosing_ = true;
            choice_highlight_ = 0;
            choice_selected_ = -1;
        } else if (name == "SetSelectEx") {
            choice_result_register_ = -1;
            choice_ex_ = true;
            choosing_ = true;
            choice_highlight_ = 0;
            choice_selected_ = -1;
        }
    }

    void advance(bool skipping = false)
    {
        if (wake_time_ || audio_wait_) {
            return;
        }
        if (waiting_for_input_ && message_.reveal_next()) {
            return;
        }
        waiting_for_input_ = false;
        if (choosing_) {
            if (choice_selected_ < 0) {
                return;
            }
            if (choice_ex_) {
                runtime_.load(choices_.at(choice_selected_).sno);
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
            const auto step = runtime_.run();
            if (step.reason == th2::VmYield::ended) {
                running_ = false;
                break;
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
                break;
            }
            if (step.reason == th2::VmYield::event) {
                try {
                    handle(step.event);
                } catch (const std::exception& error) {
                    std::cerr << step.script_name << ": "
                              << step.event.instruction.name << ": "
                              << error.what() << '\n';
                }
                if (audio_wait_) {
                    if (skipping) {
                        auto& channel =
                            audio_wait_->kind == AudioWaitKind::sound_effect
                            ? se_channels_.at(audio_wait_->channel)
                            : voice_channels_.at(audio_wait_->channel);
                        channel.stop();
                        audio_wait_.reset();
                        continue;
                    }
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
    }

    void save(int slot)
    {
        const auto path = save_path(slot);
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            return;
        }
        save_body(file);
    }

    void load(int slot)
    {
        const auto path = save_path(slot);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return;
        }
        load_body(file);
    }

    std::filesystem::path save_path(int slot) const
    {
        return std::format("save_{:02d}.sav", slot);
    }

    void save_body(std::ostream& out) const
    {
        write_u32(out, 2);  // native version

        // Script identity
        write_str(out, runtime_.script_name(), 64);
        write_i32(out, tone_);
        write_i32(out, weather_);

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
        for (const auto f : runtime_.all_game_flags()) {
            write_i32(out, f);
        }

        // Background
        write_i32(out, bg_scene_);
        write_i32(out, bg_is_visual_ ? 1 : 0);

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
        }

        // Overlays
        std::uint32_t overlay_count = 0;
        for (const auto& ov : overlays_) {
            if (ov) {
                ++overlay_count;
            }
        }
        write_u32(out, overlay_count);

        // BGM
        write_i32(out, bgm_track_);
        write_i32(out, bgm_loop_ ? 1 : 0);
        write_i32(out, bgm_volume_);

        // SE transient
        std::uint32_t se_count = 0;
        for (const auto& ch : transient_se_) {
            if (ch.playing()) {
                ++se_count;
            }
        }
        write_u32(out, se_count);

        // SE channels - full state for each active channel
        std::uint32_t se_ch_count = 0;
        for (std::size_t i = 0; i < se_channels_.size(); ++i) {
            if (se_channels_[i].playing()) {
                ++se_ch_count;
            }
        }
        write_u32(out, se_ch_count);
        for (std::size_t i = 0; i < se_channels_.size(); ++i) {
            if (se_channels_[i].playing()) {
                write_u32(out, static_cast<std::uint32_t>(i));
                write_i32(out, se_sound_[i]);
                write_i32(out, se_loop_[i] ? 1 : 0);
                write_i32(out, se_volume_[i]);
            }
        }

        // Voice channels - full state for each active channel
        std::uint32_t voice_count = 0;
        for (std::size_t i = 0; i < voice_channels_.size(); ++i) {
            if (voice_channels_[i].playing()) {
                ++voice_count;
            }
        }
        write_u32(out, voice_count);
        for (std::size_t i = 0; i < voice_channels_.size(); ++i) {
            if (voice_channels_[i].playing()) {
                write_u32(out, static_cast<std::uint32_t>(i));
                write_i32(out, voice_sound_[i]);
                write_i32(out, voice_character_[i]);
                write_i32(out, voice_scenario_[i]);
                write_i32(out, voice_volume_[i]);
                write_i32(out, voice_loop_[i] ? 1 : 0);
            }
        }

        // VM wait state
        write_i32(out, wake_time_.has_value() ? 1 : 0);
        if (wake_time_) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *wake_time_ - std::chrono::steady_clock::now()).count();
            write_i64(out, remaining > 0 ? remaining : 0);
        }
        write_i32(out, audio_wait_.has_value() ? 1 : 0);
        if (audio_wait_) {
            write_i32(out, audio_wait_->kind == AudioWaitKind::sound_effect ? 1 : 2);
            write_u32(out, static_cast<std::uint32_t>(audio_wait_->channel));
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

        // Backlog state. Depth 0 is the current message; 1 is newest history.
        write_u32(out, static_cast<std::uint32_t>(backlog_.size()));
        for (const auto& entry : backlog_) {
            write_u32(out, static_cast<std::uint32_t>(entry.text.size()));
            out.write(entry.text.data(),
                      static_cast<std::streamsize>(entry.text.size()));
        }
        write_i32(out, backlog_depth_);
        write_i32(out, ui_mode_ == UiMode::backlog ? 1 : 0);
    }

    void load_body(std::istream& in)
    {
        const auto version = read_u32(in);
        if (version < 1) {
            return;
        }

        // Stop all audio before restore
        bgm_.stop();
        for (auto& ch : transient_se_) { ch.stop(); }
        for (auto& ch : se_channels_) { ch.stop(); }
        for (auto& ch : voice_channels_) { ch.stop(); }
        audio_wait_.reset();
        wake_time_.reset();
        bgm_track_ = -1;
        ui_mode_ = UiMode::game;
        message_visible_ = true;

        // Script identity
        const auto script_name = read_str(in, 64);
        runtime_.load(script_name);
        tone_ = read_i32(in);
        weather_ = read_i32(in);

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
        for (std::size_t i = 0; i < 1024; ++i) {
            runtime_.set_game_flag(i, read_i32(in));
        }
        runtime_.vm_restore(regs, stack_data, pc);

        // Background
        bg_scene_ = read_i32(in);
        bg_is_visual_ = read_i32(in) != 0;
        restore_background();

        // Characters
        characters_ = {};
        character_textures_ = {};
        const auto char_count = read_u32(in);
        for (std::uint32_t i = 0; i < char_count; ++i) {
            const auto number = read_i32(in);
            const auto pose = read_i32(in);
            const auto locate = read_i32(in);
            const auto layer = read_i32(in);
            const auto brightness = read_i32(in);
            const auto alpha = read_i32(in);
            auto& ch = characters_.set(
                number, pose, locate, layer, brightness, alpha);
            load_character_texture(ch);
        }

        // Overlays - reset all
        read_u32(in);
        for (auto& ov : overlays_) { ov.reset(); }

        // BGM
        bgm_track_ = read_i32(in);
        bgm_loop_ = read_i32(in) != 0;
        bgm_volume_ = read_i32(in);
        if (bgm_track_ >= 0) {
            play_bgm(bgm_track_, bgm_loop_, bgm_volume_);
        }

        // SE transient
        read_u32(in);

        // SE channels - restore active channels
        const auto se_ch_count = read_u32(in);
        for (std::uint32_t i = 0; i < se_ch_count; ++i) {
            const auto channel = static_cast<std::size_t>(read_u32(in));
            const auto sound = read_i32(in);
            const auto loop = read_i32(in) != 0;
            const auto volume = read_i32(in);
            if (channel < se_channels_.size() && sound >= 0) {
                play_se(static_cast<int>(channel), sound, loop, volume);
            }
        }

        // Voice channels - restore active channels
        const auto voice_count = read_u32(in);
        for (std::uint32_t i = 0; i < voice_count; ++i) {
            const auto channel = static_cast<std::size_t>(read_u32(in));
            const auto sound = read_i32(in);
            const auto character = read_i32(in);
            const auto scenario = read_i32(in);
            const auto volume = read_i32(in);
            const auto loop = read_i32(in) != 0;
            if (channel < voice_channels_.size()) {
                const auto name = std::format(
                    "K{:09d}_{:03d}{:03d}.OGG", scenario, sound, character);
                voice_channels_[channel].play(
                    load_audio(voice_archive_, name), loop,
                    std::clamp(volume, 0, 256) / 256.0f);
                voice_sound_[channel] = sound;
                voice_character_[channel] = character;
                voice_scenario_[channel] = scenario;
                voice_volume_[channel] = volume;
                voice_loop_[channel] = loop;
            }
        }

        // VM wait state
        if (read_i32(in)) {
            const auto remaining = read_i64(in);
            wake_time_ = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(remaining);
        } else {
            wake_time_.reset();
        }
        if (read_i32(in)) {
            const auto kind_int = read_i32(in);
            const auto channel = static_cast<std::size_t>(read_u32(in));
            audio_wait_ = AudioWait{
                kind_int == 1 ? AudioWaitKind::sound_effect
                              : AudioWaitKind::voice,
                channel};
        } else {
            audio_wait_.reset();
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

        backlog_.clear();
        backlog_depth_ = 0;
        if (version >= 2) {
            const auto backlog_count = read_u32(in);
            backlog_.reserve(backlog_count);
            for (std::uint32_t i = 0; i < backlog_count; ++i) {
                const auto size = read_u32(in);
                std::string history_text(size, '\0');
                if (size > 0) {
                    in.read(history_text.data(),
                            static_cast<std::streamsize>(size));
                }
                backlog_.push_back({std::move(history_text)});
            }
            backlog_depth_ = std::clamp(
                read_i32(in), 0, static_cast<int>(backlog_.size()));
            if (read_i32(in) != 0) {
                ui_mode_ = UiMode::backlog;
            }
        }
    }

    void write_u32(std::ostream& out, std::uint32_t value) const
    {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
        out.put(static_cast<char>((value >> 16) & 0xFF));
        out.put(static_cast<char>((value >> 24) & 0xFF));
    }

    void write_i32(std::ostream& out, std::int32_t value) const
    {
        write_u32(out, static_cast<std::uint32_t>(value));
    }

    void write_i64(std::ostream& out, std::int64_t value) const
    {
        write_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFF));
        write_u32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFF));
    }

    void write_str(std::ostream& out, std::string_view str,
                   std::size_t padded_size) const
    {
        const auto len = std::min(str.size(), padded_size);
        out.write(str.data(), static_cast<std::streamsize>(len));
        for (std::size_t i = len; i < padded_size; ++i) {
            out.put('\0');
        }
    }

    std::uint32_t read_u32(std::istream& in) const
    {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(
                static_cast<unsigned char>(in.get())) << shift;
        }
        return value;
    }

    std::int32_t read_i32(std::istream& in) const
    {
        return static_cast<std::int32_t>(read_u32(in));
    }

    std::int64_t read_i64(std::istream& in) const
    {
        const auto low = static_cast<std::int64_t>(read_u32(in));
        const auto high = static_cast<std::int64_t>(read_u32(in));
        return low | (high << 32);
    }

    std::string read_str(std::istream& in, std::size_t size) const
    {
        std::string result(size, '\0');
        in.read(result.data(), static_cast<std::streamsize>(size));
        const auto null_pos = result.find('\0');
        if (null_pos != std::string::npos) {
            result.resize(null_pos);
        }
        return result;
    }

    // --- UI Methods ---

    void push_backlog()
    {
        if (message_.empty()) return;
        const auto& text = message_.visible();
        if (!backlog_.empty() && backlog_.back().text == text) return;
        backlog_.push_back({text});
        if (backlog_.size() > 256) backlog_.erase(backlog_.begin());
    }

    void open_system_menu()
    {
        if (choosing_) return;
        ui_mode_ = UiMode::system_menu;
        menu_highlight_ = 4;
    }

    void close_system_menu() { ui_mode_ = UiMode::game; }

    void open_backlog()
    {
        if (choosing_) return;
        ui_mode_ = UiMode::backlog;
        backlog_depth_ = std::min(
            1, static_cast<int>(backlog_.size()));
    }

    void close_backlog()
    {
        backlog_depth_ = 0;
        ui_mode_ = UiMode::game;
    }

    void execute_menu_item(int index)
    {
        switch (index) {
        case 0: save(0); break;
        case 1: load(0); break;
        case 2: message_visible_ = !message_visible_; break;
        case 3: break;
        case 4: break;
        }
    }

    void draw_system_menu()
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

        // 4 main buttons from sys0110.tga
        // Layout: Save(0,0) Load(400,0) Hide(0,246) Settings(400,246)
        // Each: w=400, h=82, 3 states stacked vertically (0,82,164)
        const int btn_x[4] = {0, 400, 0, 400};
        const int btn_y[4] = {0, 0, 246, 246};
        const int dst_x[4] = {200, 200, 200, 200};
        const int dst_y[4] = {112, 200, 288, 376};

        for (int i = 0; i < 4; ++i) {
            const int state = (i == menu_highlight_) ? 82 : 0;
            const SDL_FRect src{
                static_cast<float>(btn_x[i]),
                static_cast<float>(btn_y[i] + state), 400.0f, 82.0f};
            const SDL_FRect dst{
                static_cast<float>(dst_x[i]),
                static_cast<float>(dst_y[i]), 400.0f, 82.0f};
            if (ui_sys_menu_btns_) {
                SDL_RenderTexture(renderer_, ui_sys_menu_btns_.get(),
                                  &src, &dst);
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

        // Close button from sys0111.tga (src: 0, state*32, 188, 32)
        const int cs = (menu_highlight_ == 4) ? 32 : 0;
        const SDL_FRect csrc{0.0f, static_cast<float>(cs), 188.0f, 32.0f};
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

    void handle_system_menu_input(const SDL_Event& event)
    {
        const int dst_x[5] = {200, 200, 200, 200, 306};
        const int dst_y[5] = {112, 200, 288, 376, 480};
        const int dst_w[5] = {400, 400, 400, 400, 188};
        const int dst_h[5] = {82, 82, 82, 82, 32};

        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                close_system_menu();
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE) {
                const int item = menu_highlight_;
                close_system_menu();
                execute_menu_item(item);
            } else if (event.key.key == SDLK_UP) {
                menu_highlight_ = (menu_highlight_ - 1 + 5) % 5;
            } else if (event.key.key == SDLK_DOWN) {
                menu_highlight_ = (menu_highlight_ + 1) % 5;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
                close_system_menu();
            } else if (event.button.button == SDL_BUTTON_LEFT) {
                const float mx = event.button.x; const float my = event.button.y;
                for (int i = 0; i < 5; ++i) {
                    if (mx >= dst_x[i] && mx < dst_x[i] + dst_w[i]
                        && my >= dst_y[i] && my < dst_y[i] + dst_h[i]) {
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
                    && my >= dst_y[i] && my < dst_y[i] + dst_h[i]) {
                    menu_highlight_ = i;
                    break;
                }
            }
        }
    }

    void draw_backlog()
    {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 128);
        const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
        SDL_RenderFillRect(renderer_, &game_area);

        std::string_view selected = message_.visible();
        if (backlog_depth_ > 0 && backlog_depth_ <= static_cast<int>(backlog_.size())) {
            selected = backlog_[
                backlog_.size() - static_cast<std::size_t>(backlog_depth_)].text;
        }

        float y = 72.0f;
        for (const auto& line : display_lines(selected)) {
            font_.draw(renderer_, 54.0f, y + 2.0f, line, 0, 0, 0);
            font_.draw(renderer_, 52.0f, y, line, 255, 144, 32);
            y += 31.0f;
            if (y > 535.0f) {
                break;
            }
        }
    }

    void draw_sidebar()
    {
        if (!ui_sidebar_track_ || !ui_sidebar_btns_) return;

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
            const SDL_FRect hdl_src{22.0f, 0.0f, 22.0f, 30.0f};
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
            const float state_x = i == sidebar_hover_ ? 44.0f : 22.0f;
            const SDL_FRect src{
                state_x, static_cast<float>(button.source_y),
                22.0f, static_cast<float>(button.h)};
            const SDL_FRect dst{
                776.0f, static_cast<float>(button.y),
                22.0f, static_cast<float>(button.h)};
            SDL_RenderTexture(renderer_, ui_sidebar_btns_.get(),
                              &src, &dst);
        }
    }

    void update_sidebar_hover(float x, float y)
    {
        sidebar_hover_ = -1;
        if (x < 776.0f || x >= 798.0f) {
            return;
        }
        static constexpr std::array button_y{
            271, 312, 353, 376, 399, 422, 445, 468,
        };
        static constexpr std::array button_h{
            36, 36, 20, 20, 20, 20, 20, 20,
        };
        for (int i = 0; i < static_cast<int>(button_y.size()); ++i) {
            if (y >= button_y[i] && y < button_y[i] + button_h[i]) {
                sidebar_hover_ = i;
                return;
            }
        }
    }

    bool handle_sidebar_click(float x, float y)
    {
        if (x < 776.0f || x >= 798.0f) {
            return false;
        }
        if (y >= 10.0f && y < 265.0f && !backlog_.empty()) {
            const float ratio = std::clamp((y - 10.0f) / 255.0f, 0.0f, 1.0f);
            backlog_depth_ = std::clamp(
                static_cast<int>(std::lround(
                    (1.0f - ratio) * static_cast<float>(backlog_.size()))),
                0, static_cast<int>(backlog_.size()));
            ui_mode_ = backlog_depth_ == 0 ? UiMode::game : UiMode::backlog;
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
                if (backlog_depth_ < static_cast<int>(backlog_.size())) {
                    ++backlog_depth_;
                    ui_mode_ = UiMode::backlog;
                }
                break;
            case 1:
                if (backlog_depth_ > 1) {
                    --backlog_depth_;
                } else if (backlog_depth_ == 1) {
                    close_backlog();
                } else {
                    advance();
                }
                break;
            case 2: save(0); break;
            case 3: load(0); break;
            case 4: break;
            case 5: break;
            case 6: open_system_menu(); break;
            case 7: save(0); break;
            }
            return true;
        }
        return true;
    }

    void handle_backlog_input(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                close_backlog();
            } else if (event.key.key == SDLK_UP) {
                if (backlog_depth_ < static_cast<int>(backlog_.size()))
                    ++backlog_depth_;
            } else if (event.key.key == SDLK_DOWN) {
                if (backlog_depth_ > 1) --backlog_depth_;
                else close_backlog();
            } else if (event.key.key == SDLK_PAGEUP) {
                if (backlog_depth_ < static_cast<int>(backlog_.size()))
                    ++backlog_depth_;
            } else if (event.key.key == SDLK_PAGEDOWN) {
                if (backlog_depth_ > 1) --backlog_depth_;
                else close_backlog();
            } else if (event.key.key == SDLK_HOME) {
                backlog_depth_ = static_cast<int>(backlog_.size());
            } else if (event.key.key == SDLK_END) {
                close_backlog();
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
                close_backlog();
                return;
            }
            if (handle_sidebar_click(event.button.x, event.button.y)) {
                return;
            }
            close_backlog();
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (event.wheel.y > 0) {
                if (backlog_depth_ < static_cast<int>(backlog_.size()))
                    ++backlog_depth_;
            } else {
                if (backlog_depth_ > 1) --backlog_depth_;
                else close_backlog();
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            update_sidebar_hover(event.motion.x, event.motion.y);
        }
    }

    void draw_click_indicator()
    {
        if (!waiting_for_input_ || !message_visible_ || message_.empty()) return;

        const bool end_of_block =
            message_.revealed_count() >= message_.segments().size();
        auto& tex = end_of_block ? ui_keywait_ : ui_pageend_;
        if (!tex) return;

        const auto lines = display_lines(message_.visible());
        if (lines.empty()) return;
        const auto& last_line = lines.back();
        float width = 0;
        for (unsigned char c : last_line)
            width += (c >= 0x20 && c <= 0x7E) ? 12.0f : 24.0f;

        const float x = 52.0f + width + 4.0f;
        const float y = 72.0f
            + (std::min(lines.size(), static_cast<std::size_t>(15)) - 1)
            * 31.0f - 2.0f;

        // Time-based 30fps animation matching original GlobalCount/2%30 (1s cycle)
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const int frame = (ms / 33) % 30;
        const SDL_FRect src{frame * 40.0f, 0.0f, 40.0f, 40.0f};
        const SDL_FRect dst{x, y, 36.0f, 36.0f};
        SDL_RenderTexture(renderer_, tex.get(), &src, &dst);
    }

    void draw()
    {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (background_) {
            SDL_RenderTexture(renderer_, background_.get(), nullptr, nullptr);
        }
        for (const auto& character : characters_.ordered()) {
            auto& loaded = character_texture(character.number);
            if (!loaded.texture) {
                continue;
            }
            const auto brightness = static_cast<Uint8>(
                std::clamp(character.brightness * 2, 0, 255));
            SDL_SetTextureColorMod(
                loaded.texture.get(), brightness, brightness, brightness);
            SDL_SetTextureAlphaMod(
                loaded.texture.get(),
                static_cast<Uint8>(std::clamp(character.alpha, 0, 256) * 255 / 256));
            SDL_FRect destination{
                static_cast<float>(th2::character_offset(character.locate)),
                0.0f, 800.0f, 600.0f};
            SDL_RenderTexture(renderer_, loaded.texture.get(), nullptr, &destination);
        }
        for (const auto& overlay : overlays_) {
            if (overlay) {
                SDL_RenderTexture(renderer_, overlay.get(), nullptr, nullptr);
            }
        }
        if (ui_mode_ != UiMode::backlog && message_visible_ && !message_.empty()) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 16, 150);
            SDL_RenderFillRect(renderer_, nullptr);
            const auto lines = display_lines(message_.visible());
            float y = 72.0f;
            for (const auto& line : lines) {
                font_.draw(renderer_, 54.0f, y + 2.0f, line, 0, 0, 0);
                font_.draw(renderer_, 52.0f, y, line);
                y += 31.0f;
                if (y > 535.0f) {
                    break;
                }
            }
        }
        if (ui_mode_ != UiMode::backlog
            && message_visible_ && choosing_ && !choices_.empty()) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            const float top = choice_y_start() - 8.0f;
            const float height = choices_.size() * 31.0f + 16.0f;
            SDL_FRect choice_bg{0.0f, top, 800.0f, height};
            SDL_SetRenderDrawColor(renderer_, 0, 0, 16, 180);
            SDL_RenderFillRect(renderer_, &choice_bg);
            float y = choice_y_start();
            for (int i = 0; i < static_cast<int>(choices_.size()); ++i) {
                const auto highlighted = i == choice_highlight_;
                font_.draw(
                    renderer_, 54.0f, y + 2.0f, choices_[i].text, 0, 0, 0);
                font_.draw(
                    renderer_, 52.0f, y, choices_[i].text,
                    highlighted ? 255 : 128,
                    highlighted ? 255 : 128,
                    highlighted ? 255 : 128);
                y += 31.0f;
            }
        }
        if (ui_mode_ == UiMode::game) {
            draw_click_indicator();
        }
        if (ui_mode_ == UiMode::system_menu) {
            draw_system_menu();
        } else if (ui_mode_ == UiMode::backlog) {
            draw_backlog();
        }
        draw_sidebar();
        SDL_RenderPresent(renderer_);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    try {
        return Game(argc > 1 ? argv[1] : "game-data").run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
