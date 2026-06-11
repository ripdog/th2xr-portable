#include "archive.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "config.hpp"
#include "font.hpp"
#include "image.hpp"
#include "imgui_layer.hpp"
#include "message.hpp"
#include "script_runtime.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
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

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};
using Surface = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

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
          voice_archive_(data / "voice.pak"), runtime_(scripts_), font_(fonts_),
          config_(th2::load_config(config_path_))
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
        if (config_.fullscreen) {
            SDL_SetWindowFullscreen(window_, true);
        }
        imgui_ = std::make_unique<th2::ImGuiLayer>(window_, renderer_);
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
        ui_save_bg_ = try_load("sys0200.tga");
        ui_load_bg_ = try_load("sys0300.tga");
        ui_save_rows_ = try_load("sys0201.tga");
        ui_save_rows_hover_ = try_load("sys0202.tga");
        ui_save_new_ = try_load("sys0210.tga");
        ui_save_digits_ = try_load("sys0230.tga");
        ui_save_prompt_ = try_load("sys0250.tga");
        ui_load_prompt_ = try_load("sys0350.tga");
        ui_confirm_buttons_ = try_load("sys0251.tga");
        ui_save_controls_ = try_load("sys0203.tga");
        title_background_ = try_load("t0000.tga");
        title_menu_ = try_load("t0010.tga");
        if (const auto* entry = graphics_.find("t0001.tga")) {
            Surface loaded(th2::load_image(graphics_.read(*entry), entry->name));
            title_foreground_pixels_.reset(
                SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_RGBA32));
        }
        title_mask_ = load_transition_mask(
            0x80 + 52, title_mask_width_, title_mask_height_);
        title_masked_ = Texture(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
            800, 600));
        if (title_masked_) {
            SDL_SetTextureBlendMode(title_masked_.get(), SDL_BLENDMODE_BLEND);
        }
        title_started_ = std::chrono::steady_clock::now();
    }

    ~Game()
    {
        th2::save_config(config_path_, config_);
        imgui_.reset();
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
        while (running_) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running_ = false;
                    continue;
                }
                SDL_ConvertEventToRenderCoordinates(renderer_, &event);
                imgui_->process_event(event);
                if (config_open_) {
                    if (event.type == SDL_EVENT_KEY_DOWN
                        && event.key.key == SDLK_ESCAPE) {
                        close_config();
                    }
                    continue;
                }
                if (transition_ || background_fade_) {
                    continue;
                }

                // UI mode routing
                if (ui_mode_ == UiMode::title) {
                    handle_title_input(event);
                    continue;
                }
                if (ui_mode_ == UiMode::system_menu) {
                    handle_system_menu_input(event);
                    continue;
                }
                if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
                    handle_save_load_input(event);
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
                            save_snapshot_ = capture_frame_pixels();
                            save(0);
                        } else if (event.key.key == SDLK_F7) {
                            save_snapshot_ = capture_frame_pixels();
                            open_save_load(UiMode::load);
                        } else if (event.key.key == SDLK_F11) {
                            config_.fullscreen =
                                !(SDL_GetWindowFlags(window_)
                                  & SDL_WINDOW_FULLSCREEN);
                            SDL_SetWindowFullscreen(
                                window_, config_.fullscreen);
                            th2::save_config(config_path_, config_);
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
                    if (event.wheel.y > 0
                        && config_.wheel_opens_backlog) {
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
            update_playback_modes();
            update_title();
            imgui_->new_frame();
            draw_config();
            draw();
            update_transition();
            update_background_fade();
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

    struct Transition {
        Texture previous;
        Surface previous_pixels;
        Surface next_pixels;
        Texture composite;
        std::vector<std::uint8_t> mask;
        int mask_width = 0;
        int mask_height = 0;
        int vague = 128;
        std::chrono::steady_clock::time_point started;
        std::chrono::milliseconds duration;
        int type = 1;
        bool resume_script = false;
    };

    struct BackgroundFade {
        float from = 0.0f;
        float to = 0.0f;
        std::chrono::steady_clock::time_point started;
        std::chrono::milliseconds duration;
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
    const std::filesystem::path config_path_{"toheart2-config.ini"};
    th2::GameConfig config_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<th2::ImGuiLayer> imgui_;
    Texture background_;
    int bg_scene_ = -1;
    bool bg_is_visual_ = false;
    std::array<Texture, 32> overlays_{};
    std::array<int, 32> overlay_layers_{};
    th2::Characters characters_;
    std::array<CharacterTexture, 32> character_textures_{};
    th2::AudioChannel bgm_;
    int bgm_track_ = -1;
    bool bgm_loop_ = false;
    int bgm_volume_ = 255;
    std::array<th2::AudioChannel, 8> transient_se_{};
    std::array<int, 8> transient_se_volume_{};
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

    float bgm_gain(int volume) const
    {
        return std::clamp(volume, 0, 255) / 255.0f
            * config_.bgm_volume / 256.0f;
    }

    float se_gain(int volume) const
    {
        return std::clamp(volume, 0, 255) / 255.0f
            * config_.se_volume / 256.0f;
    }

    std::size_t voice_character_index(int character) const
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

    float voice_gain(int volume, int character) const
    {
        return std::clamp(volume, 0, 256) / 256.0f
            * config_.voice_volume / 256.0f
            * config_.character_voice_volume[
                voice_character_index(character)] / 256.0f;
    }

    void apply_audio_gains()
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

    // UI textures (from GRP.PAK)
    Texture ui_sys_menu_bg_;       // sys0100.tga
    Texture ui_sys_menu_btns_;     // sys0110.tga
    Texture ui_sys_cancel_;        // sys0111.tga
    Texture ui_sidebar_track_;     // sys0000.tga
    Texture ui_sidebar_btns_;      // sys0001.tga
    Texture ui_keywait_;           // sys0011.tga (mid-page cursor)
    Texture ui_pageend_;           // sys0010.tga (end-of-page cursor)
    Texture ui_save_bg_;
    Texture ui_load_bg_;
    Texture ui_save_rows_;
    Texture ui_save_rows_hover_;
    Texture ui_save_new_;
    Texture ui_save_digits_;
    Texture ui_save_prompt_;
    Texture ui_load_prompt_;
    Texture ui_confirm_buttons_;
    Texture ui_save_controls_;
    Texture title_background_;
    Texture title_menu_;
    Surface title_foreground_pixels_;
    Texture title_masked_;
    std::vector<std::uint8_t> title_mask_;
    int title_mask_width_ = 0;
    int title_mask_height_ = 0;
    th2::Message message_;
    bool message_ends_block_ = true;
    int tone_ = 0;
    int weather_ = 0;
    bool running_ = true;
    bool waiting_for_input_ = false;
    std::optional<std::chrono::steady_clock::time_point> wake_time_;
    std::optional<AudioWait> audio_wait_;
    std::optional<Transition> transition_;
    std::optional<BackgroundFade> background_fade_;
    float background_darkness_ = 0.0f;
    std::chrono::steady_clock::time_point skip_next_time_{};
    std::optional<std::chrono::steady_clock::time_point> auto_next_time_;
    std::string current_line_key_;

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
    enum class UiMode { title, game, system_menu, backlog, save, load };
    UiMode ui_mode_ = UiMode::title;
    UiMode save_return_mode_ = UiMode::game;
    int title_highlight_ = 0;
    std::chrono::steady_clock::time_point title_started_{};
    std::optional<std::chrono::steady_clock::time_point> title_exit_started_;
    bool title_exit_game_ = false;
    int menu_highlight_ = 0;
    struct BacklogEntry { std::string text; };
    std::vector<BacklogEntry> backlog_;
    int backlog_depth_ = 0;
    int sidebar_hover_ = -1;
    bool message_visible_ = true;
    int save_page_ = 0;
    int save_hover_ = -1;
    int save_confirm_slot_ = -1;
    int newest_save_slot_ = -1;
    Surface save_snapshot_;
    std::array<Texture, 10> save_thumbnails_{};
    bool config_open_ = false;
    bool auto_mode_ = false;
    bool skip_mode_ = false;

    std::string current_read_key() const
    {
        if (current_line_key_.empty() || message_.empty()) {
            return {};
        }
        return current_line_key_ + ':'
            + std::to_string(message_.revealed_count());
    }

    bool current_text_is_read() const
    {
        const auto key = current_read_key();
        return !key.empty() && config_.read_lines.contains(key);
    }

    void mark_current_text_read()
    {
        const auto key = current_read_key();
        if (!key.empty() && config_.read_lines.insert(key).second) {
            th2::save_config(config_path_, config_);
        }
    }

    bool voice_playing() const
    {
        return std::any_of(
            voice_channels_.begin(), voice_channels_.end(),
            [](const th2::AudioChannel& channel) {
                return channel.playing();
            });
    }

    void update_playback_modes()
    {
        if (config_open_ || ui_mode_ != UiMode::game || choosing_
            || transition_ || background_fade_ || wake_time_ || audio_wait_) {
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
        if (!auto_mode_ || !waiting_for_input_ || voice_playing()) {
            auto_next_time_.reset();
            return;
        }
        if (!auto_next_time_) {
            const int delay = config_.auto_skip_read && current_text_is_read()
                ? 40
                : (message_ends_block_
                    ? config_.auto_page_ms : config_.auto_line_ms);
            auto_next_time_ = now + std::chrono::milliseconds(delay);
        } else if (now >= *auto_next_time_) {
            auto_next_time_.reset();
            advance();
        }
    }

    struct SaveMetadata {
        bool exists = false;
        std::time_t timestamp = 0;
        std::string message;
    };
    std::array<SaveMetadata, 10> visible_saves_{};

    float choice_y_start() const
    {
        if (!message_.empty()) {
            return 104.0f;
        }
        return 468.0f;
    }

    void skip()
    {
        if (choosing_ || transition_ || background_fade_) {
            return;
        }
        if (waiting_for_input_ && !config_.skip_unread
            && !current_text_is_read()) {
            skip_mode_ = false;
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
            mark_current_text_read();
            if (message_.reveal_next() && message_.has_hidden_segments()) {
                auto_next_time_.reset();
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

    Surface capture_frame_pixels()
    {
        SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
        if (!surface) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        if (!converted) {
            throw std::runtime_error(SDL_GetError());
        }
        return Surface(converted);
    }

    Texture texture_from_surface(SDL_Surface* surface)
    {
        SDL_Texture* raw = SDL_CreateTextureFromSurface(renderer_, surface);
        if (!raw) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(raw, SDL_BLENDMODE_BLEND);
        return Texture(raw);
    }

    std::vector<std::uint8_t> load_transition_mask(
        int type, int& width, int& height)
    {
        const auto name = std::format("f0{:03d}.bmp", type & 0x7f);
        const auto* entry = graphics_.find(name);
        if (!entry) {
            throw std::runtime_error("transition mask not found: " + name);
        }
        Surface surface(th2::load_image(graphics_.read(*entry), entry->name));
        width = surface->w;
        height = surface->h;
        std::array<std::uint8_t, 256> curve{};
        for (int i = 0; i < 256; ++i) {
            curve[i] = static_cast<std::uint8_t>(i);
        }
        std::string_view curve_name;
        if (type & 0x100) {
            curve_name = type & 0x200 ? "rev_accel1.AMP"
                : type & 0x400 ? "rev_accel2.AMP" : "rev.AMP";
        } else if (type & 0x200) {
            curve_name = "accel1.AMP";
        } else if (type & 0x400) {
            curve_name = "accel2.AMP";
        }
        if (!curve_name.empty()) {
            const auto* curve_entry = graphics_.find(curve_name);
            if (!curve_entry) {
                throw std::runtime_error(
                    "transition curve not found: " + std::string(curve_name));
            }
            const auto bytes = graphics_.read(*curve_entry);
            if (bytes.size() != curve.size()) {
                throw std::runtime_error(
                    "invalid transition curve: " + std::string(curve_name));
            }
            std::copy(bytes.begin(), bytes.end(), curve.begin());
        }

        std::vector<std::uint8_t> mask(
            static_cast<std::size_t>(width) * height);
        const bool flip_x = type & 0x800;
        const bool flip_y = type & 0x1000;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                Uint8 r = 0;
                Uint8 g = 0;
                Uint8 b = 0;
                Uint8 a = 0;
                SDL_ReadSurfacePixel(
                    surface.get(), flip_x ? width - x - 1 : x,
                    flip_y ? height - y - 1 : y, &r, &g, &b, &a);
                mask[static_cast<std::size_t>(y) * width + x] = curve[b];
            }
        }
        return mask;
    }

    void begin_transition(
        int type, int frames, int vague, bool resume_script)
    {
        if (type < 0) {
            return;
        }
        const int effective_frames = frames > 0 ? frames : 30;
        auto previous_pixels = capture_frame_pixels();
        auto previous = texture_from_surface(previous_pixels.get());
        Transition transition{
            std::move(previous),
            std::move(previous_pixels),
            {},
            {},
            {},
            0,
            0,
            vague >= 0 ? vague : 128,
            std::chrono::steady_clock::now(),
            std::chrono::milliseconds(effective_frames * 1000 / 60),
            type,
            resume_script,
        };
        if (type >= 0x80) {
            transition.mask = load_transition_mask(
                type, transition.mask_width, transition.mask_height);
        }
        transition_ = std::move(transition);
    }

    void update_transition()
    {
        if (!transition_) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now()
            - transition_->started;
        if (elapsed < transition_->duration) {
            return;
        }
        const bool resume_script = transition_->resume_script;
        transition_.reset();
        if (resume_script) {
            advance();
        }
    }

    void draw_pattern_transition(float progress)
    {
        auto& transition = *transition_;
        if (!transition.next_pixels) {
            transition.next_pixels = capture_frame_pixels();
            if (transition.next_pixels->w != transition.previous_pixels->w
                || transition.next_pixels->h != transition.previous_pixels->h) {
                throw std::runtime_error("transition frame size changed");
            }
            SDL_Texture* raw = SDL_CreateTexture(
                renderer_, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STREAMING,
                transition.next_pixels->w, transition.next_pixels->h);
            if (!raw) {
                throw std::runtime_error(SDL_GetError());
            }
            transition.composite.reset(raw);
        }

        const int width = transition.next_pixels->w;
        const int height = transition.next_pixels->h;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * height * 4);
        const auto* previous =
            static_cast<const std::uint8_t*>(transition.previous_pixels->pixels);
        const auto* next =
            static_cast<const std::uint8_t*>(transition.next_pixels->pixels);
        const int vague = std::clamp(transition.vague, 1, 256);
        const int blend_offset = static_cast<int>(
            progress * static_cast<float>(256 + vague));

        for (int y = 0; y < height; ++y) {
            const int mask_y = y * transition.mask_height / height;
            for (int x = 0; x < width; ++x) {
                const int mask_x = x * transition.mask_width / width;
                const int mask = transition.mask[
                    static_cast<std::size_t>(mask_y) * transition.mask_width
                    + mask_x];
                const int alpha = std::clamp(
                    (mask + blend_offset - 256) * 256 / vague, 0, 255);
                const auto source_offset =
                    static_cast<std::size_t>(y) * transition.next_pixels->pitch
                    + static_cast<std::size_t>(x) * 4;
                const auto previous_offset =
                    static_cast<std::size_t>(y) * transition.previous_pixels->pitch
                    + static_cast<std::size_t>(x) * 4;
                const auto output_offset =
                    (static_cast<std::size_t>(y) * width + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    pixels[output_offset + channel] = static_cast<std::uint8_t>(
                        (previous[previous_offset + channel] * (255 - alpha)
                         + next[source_offset + channel] * alpha)
                        / 255);
                }
                pixels[output_offset + 3] = 255;
            }
        }
        if (!SDL_UpdateTexture(
                transition.composite.get(), nullptr, pixels.data(), width * 4)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_RenderTexture(
            renderer_, transition.composite.get(), nullptr, nullptr);
    }

    void begin_background_fade(float target, int frames)
    {
        const int effective_frames = frames > 0 ? frames : 30;
        background_fade_ = BackgroundFade{
            background_darkness_,
            std::clamp(target, 0.0f, 1.0f),
            std::chrono::steady_clock::now(),
            std::chrono::milliseconds(effective_frames * 1000 / 60),
        };
    }

    void update_background_fade()
    {
        if (!background_fade_) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now()
            - background_fade_->started;
        const float progress = std::clamp(
            std::chrono::duration<float>(elapsed).count()
                / std::chrono::duration<float>(background_fade_->duration).count(),
            0.0f, 1.0f);
        background_darkness_ = background_fade_->from
            + (background_fade_->to - background_fade_->from) * progress;
        if (progress >= 1.0f) {
            background_darkness_ = background_fade_->to;
            background_fade_.reset();
            advance();
        }
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
                se_gain(volume));
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
    }

    void play_bgm(int music, bool loop, int volume)
    {
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
                voice_gain(volume, character));
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
        const bool keep_characters = number(event, 4) > 0;
        if (!keep_characters) {
            characters_.clear();
            character_textures_ = {};
        }
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
                begin_transition(
                    number(event, 0), number(event, 3), number(event, 6), true);
                set_background(event);
            }
        } else if (name == "V" || name == "VT") {
            begin_transition(
                number(event, 0), number(event, 3), number(event, 7), true);
            set_visual(event);
        } else if (name == "FI") {
            begin_background_fade(0.0f, 30);
        } else if (name == "FIF") {
            begin_background_fade(0.0f, number(event, 0));
        } else if (name == "FO") {
            begin_background_fade(1.0f, 30);
        } else if (name == "FOF") {
            begin_background_fade(1.0f, number(event, 0));
        } else if (name == "FB") {
            const float average = (
                number(event, 0) + number(event, 1) + number(event, 2))
                / (3.0f * 128.0f);
            begin_background_fade(1.0f - average, number(event, 3));
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
                overlay_layers_[slot] = number(event, 3);
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
            current_line_key_ = runtime_.script_name() + ':'
                + std::to_string(runtime_.vm_pc());
            message_ends_block_ = number(event, 1) == 2;
            waiting_for_input_ = true;
            auto_next_time_.reset();
        } else if (name == "AddMessage2") {
            message_.append(text(event, 0));
            current_line_key_ = runtime_.script_name() + ':'
                + std::to_string(runtime_.vm_pc());
            message_ends_block_ = number(event, 1) == 2;
            waiting_for_input_ = true;
            auto_next_time_.reset();
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
            bgm_volume_ = number(event, 0);
            bgm_.set_gain(bgm_gain(bgm_volume_));
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
                se_volume_[channel] = number(event, 1);
                se_channels_[channel].set_gain(se_gain(se_volume_[channel]));
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
        if (wake_time_ || audio_wait_ || transition_ || background_fade_) {
            return;
        }
        if (waiting_for_input_) {
            mark_current_text_read();
        }
        if (waiting_for_input_ && message_.reveal_next()) {
            if (message_.has_hidden_segments()) {
                auto_next_time_.reset();
                return;
            }
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
                if (transition_) {
                    break;
                }
                if (background_fade_) {
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
        file.close();
        save_preview(slot);
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

    std::filesystem::path thumbnail_path(int slot) const
    {
        return std::format("save_{:02d}.bmp", slot);
    }

    std::filesystem::path metadata_path(int slot) const
    {
        return std::format("save_{:02d}.meta", slot);
    }

    void save_preview(int slot)
    {
        if (save_snapshot_) {
            Surface thumbnail(SDL_CreateSurface(80, 60, SDL_PIXELFORMAT_RGBA32));
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
            metadata << std::time(nullptr) << '\n' << excerpt.substr(0, 18) << '\n';
        }
    }

    SaveMetadata read_save_metadata(int slot) const
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
            std::getline(metadata, result.message);
            result.timestamp = static_cast<std::time_t>(timestamp);
        }
        if (result.timestamp == 0) {
            const auto written = std::filesystem::last_write_time(save_path(slot));
            result.timestamp = std::chrono::system_clock::to_time_t(
                std::chrono::file_clock::to_sys(written));
        }
        return result;
    }

    void refresh_save_page()
    {
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

    void save_body(std::ostream& out) const
    {
        write_u32(out, 4);  // native version

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
                    voice_gain(volume, character));
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
        message_ends_block_ = version >= 3 ? read_i32(in) != 0 : true;
        current_line_key_.clear();
        auto_mode_ = false;
        skip_mode_ = false;
        if (version >= 4) {
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
            th2::save_config(config_path_, config_);
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
        save_snapshot_ = capture_frame_pixels();
        begin_transition(1, 12, 128, false);
        ui_mode_ = UiMode::system_menu;
        menu_highlight_ = 4;
    }

    void start_new_game()
    {
        runtime_.load("EV_0301MORNING.SDT");
        ui_mode_ = UiMode::game;
        message_ = th2::Message{};
        backlog_.clear();
        characters_.clear();
        character_textures_ = {};
        background_.reset();
        bg_scene_ = -1;
        advance();
    }

    void begin_title_exit(bool start_game)
    {
        if (title_exit_started_) {
            return;
        }
        title_exit_game_ = start_game;
        title_exit_started_ = std::chrono::steady_clock::now();
    }

    void update_title()
    {
        if (!title_exit_started_) {
            return;
        }
        const auto elapsed =
            std::chrono::steady_clock::now() - *title_exit_started_;
        if (elapsed < std::chrono::milliseconds(32 * 1000 / 60)) {
            return;
        }
        const bool start_game = title_exit_game_;
        title_exit_started_.reset();
        if (start_game) {
            start_new_game();
        } else {
            running_ = false;
        }
    }

    void close_system_menu()
    {
        begin_transition(1, 12, 128, false);
        ui_mode_ = UiMode::game;
    }

    void open_config()
    {
        config_open_ = true;
    }

    void close_config()
    {
        config_open_ = false;
        th2::save_config(config_path_, config_);
    }

    void draw_config()
    {
        if (!config_open_) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(570.0f, 500.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(115.0f, 50.0f), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Panel Config", &open)) {
            if (ImGui::BeginTabBar("config-tabs")) {
                if (ImGui::BeginTabItem("Playback")) {
                    if (ImGui::Checkbox("Auto mode", &auto_mode_)
                        && auto_mode_) {
                        skip_mode_ = false;
                    }
                    ImGui::Checkbox(
                        "Auto mode skips previously read text",
                        &config_.auto_skip_read);
                    ImGui::SliderInt(
                        "Delay between lines", &config_.auto_line_ms,
                        250, 10000, "%d ms");
                    ImGui::SliderInt(
                        "Delay at page end", &config_.auto_page_ms,
                        500, 15000, "%d ms");
                    ImGui::Separator();
                    if (ImGui::Checkbox("Skip mode", &skip_mode_)
                        && skip_mode_) {
                        auto_mode_ = false;
                    }
                    ImGui::Checkbox(
                        "Allow skipping unread text", &config_.skip_unread);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    bool audio_changed = ImGui::SliderInt(
                        "Music", &config_.bgm_volume, 0, 256);
                    audio_changed |= ImGui::SliderInt(
                        "Sound effects", &config_.se_volume, 0, 256);
                    audio_changed |= ImGui::SliderInt(
                        "Voices", &config_.voice_volume, 0, 256);
                    ImGui::SeparatorText("Character voices");
                    static constexpr std::array labels{
                        "Konomi", "Manaka", "Tamaki", "Karin", "Sango",
                        "Ruri", "Yuma", "Lucy", "Yuki", "Other", "Sasara",
                    };
                    for (std::size_t i = 0; i < labels.size(); ++i) {
                        audio_changed |= ImGui::SliderInt(
                            labels[i], &config_.character_voice_volume[i],
                            0, 256);
                    }
                    if (audio_changed) {
                        apply_audio_gains();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Display & Input")) {
                    if (ImGui::Checkbox("Fullscreen", &config_.fullscreen)) {
                        SDL_SetWindowFullscreen(window_, config_.fullscreen);
                    }
                    ImGui::Checkbox(
                        "Mouse wheel opens backlog",
                        &config_.wheel_opens_backlog);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(110.0f, 0.0f))) {
                open = false;
            }
        }
        ImGui::End();
        if (!open) {
            close_config();
        }
    }

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
        case 0: open_save_load(UiMode::save); break;
        case 1: open_save_load(UiMode::load); break;
        case 2: message_visible_ = !message_visible_; break;
        case 3: open_config(); break;
        case 4: break;
        }
    }

    void open_save_load(UiMode mode)
    {
        if (!save_snapshot_) {
            save_snapshot_ = capture_frame_pixels();
        }
        save_return_mode_ =
            ui_mode_ == UiMode::title ? UiMode::title : UiMode::game;
        ui_mode_ = mode;
        save_confirm_slot_ = -1;
        save_hover_ = -1;
        refresh_save_page();
        if (newest_save_slot_ >= 0) {
            save_page_ = newest_save_slot_ / 10;
            refresh_save_page();
        }
    }

    void close_save_load()
    {
        save_confirm_slot_ = -1;
        begin_transition(1, 12, 128, false);
        ui_mode_ = save_return_mode_;
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

        // sys0111.tga stores normal, hover and pressed states horizontally.
        const float cs = menu_highlight_ == 4 ? 188.0f : 0.0f;
        const SDL_FRect csrc{cs, 0.0f, 188.0f, 32.0f};
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

    void draw_title()
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

        for (int i = 0; i < 5; ++i) {
            if (i == 3) {
                continue;
            }
            const float alpha = std::clamp(
                static_cast<float>(frame - 48 - 40 - i * 4) / 16.0f,
                0.0f, 1.0f);
            SDL_SetTextureAlphaModFloat(title_menu_.get(), alpha);
            const float source_x = i == title_highlight_ ? 188.0f : 0.0f;
            const SDL_FRect src{
                source_x, static_cast<float>(32 * i), 188.0f, 32.0f};
            const SDL_FRect dst{
                306.0f, static_cast<float>(385 + 40 * i), 188.0f, 32.0f};
            SDL_RenderTexture(renderer_, title_menu_.get(), &src, &dst);
        }
        SDL_SetTextureAlphaModFloat(title_menu_.get(), 1.0f);
    }

    void activate_title_item()
    {
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
        case 4:
            begin_title_exit(false);
            break;
        default:
            break;
        }
    }

    void handle_title_input(const SDL_Event& event)
    {
        const int frame = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - title_started_).count()
            * 60 / 1000);
        if (frame < 120 || title_exit_started_) {
            return;
        }
        const int previous_highlight = title_highlight_;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_UP) {
                do {
                    title_highlight_ = (title_highlight_ + 4) % 5;
                } while (title_highlight_ == 3);
            } else if (event.key.key == SDLK_DOWN) {
                do {
                    title_highlight_ = (title_highlight_ + 1) % 5;
                } while (title_highlight_ == 3);
            } else if (event.key.key == SDLK_RETURN
                       || event.key.key == SDLK_SPACE) {
                activate_title_item();
            } else if (event.key.key == SDLK_ESCAPE) {
                title_highlight_ = 4;
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            const float x = event.motion.x;
            const float y = event.motion.y;
            if (x >= 306.0f && x < 494.0f) {
                for (int i = 0; i < 5; ++i) {
                    if (i == 3) continue;
                    const float top = static_cast<float>(385 + 40 * i);
                    if (y >= top && y < top + 32.0f) {
                        title_highlight_ = i;
                        break;
                    }
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                   && event.button.button == SDL_BUTTON_LEFT) {
            const float x = event.button.x;
            const float y = event.button.y;
            if (x >= 306.0f && x < 494.0f) {
                for (int i = 0; i < 5; ++i) {
                    if (i == 3) continue;
                    const float top = static_cast<float>(385 + 40 * i);
                    if (y >= top && y < top + 32.0f) {
                        title_highlight_ = i;
                        activate_title_item();
                        break;
                    }
                }
            }
        }
        if (title_highlight_ != previous_highlight) {
            play_se(-1, 9108, false, 255);
        }
    }

    void draw_save_load()
    {
        auto& background = ui_mode_ == UiMode::save ? ui_save_bg_ : ui_load_bg_;
        if (background) {
            SDL_RenderTexture(renderer_, background.get(), nullptr, nullptr);
        }
        const SDL_FRect rows_dst{15.0f, 111.0f, 770.0f, 378.0f};
        if (ui_save_rows_) {
            SDL_RenderTexture(
                renderer_, ui_save_rows_.get(), nullptr, &rows_dst);
        }

        for (int i = 0; i < 10; ++i) {
            const float x = 16.0f + 390.0f * (i / 5);
            const float y = 112.0f + 76.0f * (i % 5);
            if (i == save_hover_ && ui_save_rows_hover_) {
                const SDL_FRect src{
                    390.0f * (i / 5), 76.0f * (i % 5), 380.0f, 74.0f};
                const SDL_FRect dst{x, y, 380.0f, 74.0f};
                SDL_RenderTexture(
                    renderer_, ui_save_rows_hover_.get(), &src, &dst);
            }
            if (save_thumbnails_[i]) {
                const SDL_FRect thumb{x + 16.0f, y + 6.0f, 80.0f, 60.0f};
                SDL_RenderTexture(
                    renderer_, save_thumbnails_[i].get(), nullptr, &thumb);
            }

            const int slot = save_page_ * 10 + i;
            const auto slot_text = std::format("{:03d}", slot + 1);
            font_.draw(renderer_, x + 102.0f, y + 8.0f, slot_text, 0, 0, 0);
            font_.draw(
                renderer_, x + 100.0f, y + 6.0f, slot_text, 245, 220, 190);
            if (!visible_saves_[i].exists) {
                continue;
            }

            std::tm local{};
            localtime_r(&visible_saves_[i].timestamp, &local);
            const auto date = std::format(
                "{:04d}/{:02d}/{:02d}  {:02d}:{:02d}",
                local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                local.tm_hour, local.tm_min);
            font_.draw(
                renderer_, x + 101.0f, y + 45.0f, date, 70, 25, 34);
            font_.draw(
                renderer_, x + 100.0f, y + 44.0f, date, 210, 110, 120);
            font_.draw(
                renderer_, x + 134.0f, y + 9.0f,
                visible_saves_[i].message.substr(0, 18), 255, 245, 225);
            if (slot == newest_save_slot_ && ui_save_new_) {
                const SDL_FRect badge{x + 302.0f, y + 37.0f, 56.0f, 29.0f};
                SDL_RenderTexture(
                    renderer_, ui_save_new_.get(), nullptr, &badge);
            }
        }

        font_.draw(
            renderer_, 366.0f, 80.0f,
            std::format("{:02d}/10", save_page_ + 1), 255, 245, 225);
        if (ui_save_controls_) {
            const SDL_FRect prev_src{
                0.0f, save_hover_ == 10 ? 64.0f : 0.0f, 130.0f, 32.0f};
            const SDL_FRect next_src{
                0.0f, 32.0f + (save_hover_ == 11 ? 64.0f : 0.0f),
                130.0f, 32.0f};
            const SDL_FRect prev_dst{190.0f, 72.0f, 130.0f, 32.0f};
            const SDL_FRect next_dst{482.0f, 72.0f, 130.0f, 32.0f};
            SDL_RenderTexture(
                renderer_, ui_save_controls_.get(), &prev_src,
                &prev_dst);
            SDL_RenderTexture(
                renderer_, ui_save_controls_.get(), &next_src,
                &next_dst);
        }
        if (ui_sys_cancel_) {
            const SDL_FRect src{
                save_hover_ == 12 ? 188.0f : 0.0f, 0.0f, 188.0f, 32.0f};
            const SDL_FRect dst{306.0f, 496.0f, 188.0f, 32.0f};
            SDL_RenderTexture(renderer_, ui_sys_cancel_.get(), &src, &dst);
        }

        if (save_confirm_slot_ >= 0) {
            auto& prompt =
                ui_mode_ == UiMode::save ? ui_save_prompt_ : ui_load_prompt_;
            if (prompt) {
                const SDL_FRect dst{0.0f, 246.0f, 800.0f, 142.0f};
                SDL_RenderTexture(renderer_, prompt.get(), nullptr, &dst);
            }
            const int selected = save_confirm_slot_ - save_page_ * 10;
            if (selected >= 0 && selected < 10) {
                const float x = 25.0f;
                const float y = 271.0f;
                if (save_thumbnails_[selected]) {
                    const SDL_FRect thumb{x, y + 2.0f, 80.0f, 60.0f};
                    SDL_RenderTexture(
                        renderer_, save_thumbnails_[selected].get(),
                        nullptr, &thumb);
                }
                const auto slot_text =
                    std::format("{:03d}", save_confirm_slot_ + 1);
                font_.draw(
                    renderer_, x + 102.0f, y + 4.0f,
                    slot_text, 245, 220, 190);
                font_.draw(
                    renderer_, x + 134.0f, y + 4.0f,
                    visible_saves_[selected].message.substr(0, 18),
                    255, 245, 225);

                std::tm local{};
                localtime_r(
                    &visible_saves_[selected].timestamp, &local);
                const auto date = std::format(
                    "{:04d}/{:02d}/{:02d}  {:02d}:{:02d}",
                    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                    local.tm_hour, local.tm_min);
                font_.draw(
                    renderer_, x + 100.0f, y + 36.0f,
                    date, 210, 110, 120);
            }
            if (ui_confirm_buttons_) {
                const SDL_FRect yes_src{
                    0.0f, save_hover_ == 13 ? 32.0f : 0.0f, 130.0f, 32.0f};
                const SDL_FRect no_src{
                    130.0f, save_hover_ == 14 ? 32.0f : 0.0f,
                    130.0f, 32.0f};
                const SDL_FRect yes_dst{461.0f, 320.0f, 130.0f, 32.0f};
                const SDL_FRect no_dst{606.0f, 320.0f, 130.0f, 32.0f};
                SDL_RenderTexture(
                    renderer_, ui_confirm_buttons_.get(), &yes_src,
                    &yes_dst);
                SDL_RenderTexture(
                    renderer_, ui_confirm_buttons_.get(), &no_src,
                    &no_dst);
            }
        }
    }

    int save_load_hit(float x, float y) const
    {
        if (save_confirm_slot_ >= 0) {
            if (x >= 461 && x < 591 && y >= 320 && y < 352) return 13;
            if (x >= 606 && x < 736 && y >= 320 && y < 352) return 14;
            return -1;
        }
        for (int i = 0; i < 10; ++i) {
            const float left = 16.0f + 390.0f * (i / 5);
            const float top = 112.0f + 76.0f * (i % 5);
            if (x >= left && x < left + 380 && y >= top && y < top + 74) {
                return i;
            }
        }
        if (x >= 190 && x < 320 && y >= 72 && y < 104) return 10;
        if (x >= 482 && x < 612 && y >= 72 && y < 104) return 11;
        if (x >= 306 && x < 494 && y >= 496 && y < 528) return 12;
        return -1;
    }

    void activate_save_load_item(int item)
    {
        if (item >= 0 && item < 10) {
            const int slot = save_page_ * 10 + item;
            if (ui_mode_ == UiMode::load && !visible_saves_[item].exists) {
                return;
            }
            if (ui_mode_ == UiMode::save && !visible_saves_[item].exists) {
                save(slot);
                close_save_load();
            } else {
                save_confirm_slot_ = slot;
                save_hover_ = 14;
            }
        } else if (item == 10 || item == 11) {
            save_page_ = (save_page_ + (item == 10 ? 9 : 1)) % 10;
            refresh_save_page();
        } else if (item == 12) {
            close_save_load();
        } else if (item == 13 && save_confirm_slot_ >= 0) {
            const int slot = save_confirm_slot_;
            if (ui_mode_ == UiMode::save) {
                save(slot);
                close_save_load();
            } else {
                load(slot);
                save_confirm_slot_ = -1;
                ui_mode_ = UiMode::game;
            }
        } else if (item == 14) {
            save_confirm_slot_ = -1;
            save_hover_ = -1;
        }
    }

    void handle_save_load_input(const SDL_Event& event)
    {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            const int hovered = save_load_hit(event.motion.x, event.motion.y);
            if (hovered != save_hover_) {
                save_hover_ = hovered;
                if (save_hover_ >= 0) {
                    play_se(-1, 9108, false, 255);
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
                if (save_confirm_slot_ >= 0) {
                    save_confirm_slot_ = -1;
                } else {
                    close_save_load();
                }
            } else if (event.button.button == SDL_BUTTON_LEFT) {
                activate_save_load_item(
                    save_load_hit(event.button.x, event.button.y));
            }
        } else if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                if (save_confirm_slot_ >= 0) {
                    save_confirm_slot_ = -1;
                } else {
                    close_save_load();
                }
            } else if (event.key.key == SDLK_PAGEUP) {
                activate_save_load_item(10);
            } else if (event.key.key == SDLK_PAGEDOWN) {
                activate_save_load_item(11);
            } else if (event.key.key == SDLK_RETURN
                       || event.key.key == SDLK_SPACE) {
                activate_save_load_item(save_hover_);
            }
        }
    }

    void handle_system_menu_input(const SDL_Event& event)
    {
        const int dst_x[5] = {200, 200, 200, 200, 306};
        const int dst_y[5] = {112, 200, 288, 376, 480};
        const int dst_w[5] = {400, 400, 400, 400, 188};
        const int dst_h[5] = {82, 82, 82, 82, 32};

        const int previous_highlight = menu_highlight_;
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
        if (menu_highlight_ != previous_highlight) {
            play_se(-1, 9108, false, 255);
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
            const bool active =
                (i == 4 && auto_mode_) || (i == 5 && skip_mode_);
            const float state_x = active ? 66.0f
                : (i == sidebar_hover_ ? 44.0f : 22.0f);
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
        const int previous_hover = sidebar_hover_;
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
                if (sidebar_hover_ != previous_hover) {
                    play_se(-1, 9108, false, 255);
                }
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
            case 2:
                save_snapshot_ = capture_frame_pixels();
                open_save_load(UiMode::save);
                break;
            case 3:
                save_snapshot_ = capture_frame_pixels();
                open_save_load(UiMode::load);
                break;
            case 4:
                auto_mode_ = !auto_mode_;
                if (auto_mode_) skip_mode_ = false;
                break;
            case 5:
                skip_mode_ = !skip_mode_;
                if (skip_mode_) auto_mode_ = false;
                break;
            case 6: open_config(); break;
            case 7:
                save_snapshot_ = capture_frame_pixels();
                save(0);
                break;
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
            !message_.has_hidden_segments() && message_ends_block_;
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
        if (ui_mode_ == UiMode::title) {
            draw_title();
            if (!transition_) {
                imgui_->render();
                SDL_RenderPresent(renderer_);
                return;
            }
        }
        for (std::size_t i = 0; i < overlays_.size(); ++i) {
            if (overlays_[i] && overlay_layers_[i] <= 0) {
                SDL_RenderTexture(renderer_, overlays_[i].get(), nullptr, nullptr);
            }
        }
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
        for (std::size_t i = 0; i < overlays_.size(); ++i) {
            if (overlays_[i] && overlay_layers_[i] > 0) {
                SDL_RenderTexture(renderer_, overlays_[i].get(), nullptr, nullptr);
            }
        }
        if (background_darkness_ > 0.0f) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(
                renderer_, 0, 0, 0,
                static_cast<Uint8>(background_darkness_ * 255.0f));
            const SDL_FRect game_area{0.0f, 0.0f, 800.0f, 600.0f};
            SDL_RenderFillRect(renderer_, &game_area);
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
        } else if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
            draw_save_load();
        }
        if (ui_mode_ == UiMode::game || ui_mode_ == UiMode::backlog) {
            draw_sidebar();
        }
        if (transition_) {
            const auto elapsed = std::chrono::steady_clock::now()
                - transition_->started;
            const float progress = std::clamp(
                std::chrono::duration<float>(elapsed).count()
                    / std::chrono::duration<float>(transition_->duration).count(),
                0.0f, 1.0f);
            auto draw_previous = [&](const SDL_FRect& rectangle) {
                SDL_RenderTexture(
                    renderer_, transition_->previous.get(),
                    &rectangle, &rectangle);
            };
            if (transition_->type >= 0x80) {
                draw_pattern_transition(progress);
            } else if (transition_->type == 0) {
                if (progress < 0.5f) {
                    SDL_SetTextureAlphaModFloat(
                        transition_->previous.get(), 1.0f);
                    SDL_RenderTexture(
                        renderer_, transition_->previous.get(), nullptr, nullptr);
                }
                const float midpoint = progress < 0.5f
                    ? progress * 2.0f : (1.0f - progress) * 2.0f;
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(
                    renderer_, 0, 0, 0,
                    static_cast<Uint8>(
                        std::clamp(midpoint, 0.0f, 1.0f) * 255.0f));
                SDL_RenderFillRect(renderer_, nullptr);
            } else if (transition_->type >= 2 && transition_->type <= 7) {
                SDL_SetTextureAlphaModFloat(transition_->previous.get(), 1.0f);
                const float width = 800.0f;
                const float height = 600.0f;
                if (transition_->type == 2) {
                    draw_previous({0.0f, 0.0f, width, height * (1.0f - progress)});
                } else if (transition_->type == 3) {
                    const float y = height * progress;
                    draw_previous({0.0f, y, width, height - y});
                } else if (transition_->type == 4) {
                    const float x = width * progress;
                    draw_previous({x, 0.0f, width - x, height});
                } else if (transition_->type == 5) {
                    draw_previous({0.0f, 0.0f, width * (1.0f - progress), height});
                } else if (transition_->type == 6) {
                    const float side = width * (1.0f - progress) / 2.0f;
                    draw_previous({0.0f, 0.0f, side, height});
                    draw_previous({width - side, 0.0f, side, height});
                } else {
                    const float middle = width * (1.0f - progress);
                    const float x = (width - middle) / 2.0f;
                    draw_previous({x, 0.0f, middle, height});
                }
            } else {
                SDL_SetTextureAlphaModFloat(
                    transition_->previous.get(), 1.0f - progress);
                SDL_RenderTexture(
                    renderer_, transition_->previous.get(), nullptr, nullptr);
            }
        }
        imgui_->render();
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
