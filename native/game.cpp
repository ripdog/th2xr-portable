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
#include <cstdint>
#include <exception>
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
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.key == SDLK_ESCAPE) {
                        running_ = false;
                    } else if (event.key.key == SDLK_F11) {
                        SDL_SetWindowFullscreen(
                            window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN));
                    } else if (event.key.key == SDLK_RETURN
                               || event.key.key == SDLK_SPACE) {
                        advance();
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                           && event.button.button == SDL_BUTTON_LEFT) {
                    advance();
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
    std::array<Texture, 32> overlays_{};
    th2::Characters characters_;
    std::array<CharacterTexture, 32> character_textures_{};
    th2::AudioChannel bgm_;
    std::array<th2::AudioChannel, 8> transient_se_{};
    std::array<th2::AudioChannel, 16> se_channels_{};
    std::array<th2::AudioChannel, 8> voice_channels_{};
    th2::Message message_;
    int tone_ = 0;
    int weather_ = 0;
    bool running_ = true;
    bool waiting_for_input_ = false;
    std::optional<std::chrono::steady_clock::time_point> wake_time_;
    std::optional<AudioWait> audio_wait_;
    std::chrono::steady_clock::time_point skip_next_time_{};

    void skip()
    {
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
        background_ = load_texture(
            renderer_, graphics_, std::format("v{:06d}.tga", visual));
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
            message_.set(text(event, 0));
            waiting_for_input_ = true;
        } else if (name == "AddMessage2") {
            message_.append(text(event, 0));
            waiting_for_input_ = true;
        } else if (name == "M") {
            const int music = number(event, 0);
            if (music < 0) {
                bgm_.stop();
            } else {
                const int loop = number(event, 2) < 0 ? 1 : number(event, 2);
                const int volume = number(event, 3) < 0 ? 255 : number(event, 3);
                play_bgm(music, loop != 0, volume);
            }
        } else if (name == "MS") {
            bgm_.stop();
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
            }
        } else if (name == "VW") {
            const auto channel = number(event, 0) < 0 ? 0 : number(event, 0);
            if (channel >= 0 && static_cast<std::size_t>(channel) < voice_channels_.size()
                && voice_channels_[channel].playing()) {
                audio_wait_ = AudioWait{
                    AudioWaitKind::voice, static_cast<std::size_t>(channel)};
            }
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
        while (running_ && !waiting_for_input_) {
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
            }
        }
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
        if (!message_.empty()) {
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
