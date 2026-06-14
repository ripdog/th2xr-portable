#include "archive.hpp"
#include "audio.hpp"
#include "upscaler.hpp"
#include "character.hpp"
#include "config.hpp"
#include "font.hpp"
#include "image.hpp"
#include "imgui_layer.hpp"
#include "message.hpp"
#include "player_name.hpp"
#include "script_runtime.hpp"
#include "video.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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

struct SdlSubsystem {
    SdlSubsystem()
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            throw std::runtime_error(SDL_GetError());
        }
    }
    ~SdlSubsystem() { SDL_Quit(); }

    SdlSubsystem(const SdlSubsystem&) = delete;
    SdlSubsystem& operator=(const SdlSubsystem&) = delete;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
};

struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const
    {
        SDL_DestroyRenderer(renderer);
    }
};

using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;

// Convert window-coordinate mouse events to the fixed 800x600 logical
// coordinate system used by the core game, applying 4:3 letterboxing.
void convert_event_to_logical_coordinates(
    SDL_Event& event, int window_width, int window_height)
{
    const float scale = std::min(
        window_width / 800.0f, window_height / 600.0f);
    const float logical_width = 800.0f * scale;
    const float logical_height = 600.0f * scale;
    const float offset_x = (window_width - logical_width) / 2.0f;
    const float offset_y = (window_height - logical_height) / 2.0f;

    auto convert = [&](float x, float y) {
        return std::pair{(x - offset_x) / scale, (y - offset_y) / scale};
    };

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        std::tie(event.motion.x, event.motion.y) =
            convert(event.motion.x, event.motion.y);
        event.motion.xrel /= scale;
        event.motion.yrel /= scale;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        std::tie(event.button.x, event.button.y) =
            convert(event.button.x, event.button.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        std::tie(event.wheel.mouse_x, event.wheel.mouse_y) =
            convert(event.wheel.mouse_x, event.wheel.mouse_y);
        break;
    default:
        break;
    }
}

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

struct ToneCurveSpec {
    std::string name;
    int vividness = 256;
};

Texture load_toned_texture(
    SDL_Renderer* renderer,
    const th2::Archive& image_archive,
    std::string_view image_name,
    const th2::Archive& curve_archive,
    const std::vector<ToneCurveSpec>& curves,
    Surface* pixels = nullptr)
{
    const auto* image = image_archive.find(image_name);
    if (!image) {
        throw std::runtime_error(
            "image not found: " + std::string(image_name));
    }
    Surface surface(
        th2::load_image(image_archive.read(*image), image->name));
    for (const auto& curve : curves) {
        if (curve.name.empty()) {
            th2::apply_tone_curve(surface.get(), {}, curve.vividness);
            continue;
        }
        const auto* entry = curve_archive.find(curve.name);
        if (!entry) {
            throw std::runtime_error(
                "tone curve not found: " + curve.name);
        }
        th2::apply_tone_curve(
            surface.get(), curve_archive.read(*entry), curve.vividness);
    }
    SDL_Surface* texture_surface = surface.get();
    if (pixels) {
        pixels->reset(
            SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_RGBA32));
        if (!*pixels) {
            throw std::runtime_error(SDL_GetError());
        }
        texture_surface = pixels->get();
    }
    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(renderer, texture_surface);
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
    bool just_wrapped = false;
    for (std::size_t position = 0; position < source.size();) {
        if (source[position] == '\n') {
            if (!line.empty() || !just_wrapped) {
                lines.push_back(line);
            }
            line.clear();
            just_wrapped = false;
            ++position;
            continue;
        }
        line.push_back(source[position++]);
        just_wrapped = false;
        // A leading separator after \k is visible immediately. The original
        // renderer does not wrap until the next printable glyph establishes
        // that the line is over width.
        if (line.size() >= 60 && line.back() != ' ') {
            const auto space = line.find_last_of(' ');
            if (space != std::string::npos && space > 35) {
                lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
            } else {
                lines.push_back(line);
                line.clear();
            }
            just_wrapped = line.empty();
        }
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::string interpret_newlines(std::string text)
{
    for (std::size_t position = 0;
         (position = text.find("\\n", position)) != std::string::npos;) {
        text.replace(position, 2, "\n");
        ++position;
    }
    return text;
}

bool clip_texture_source(
    SDL_Texture* texture, SDL_FRect& source, SDL_FRect& destination)
{
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(texture, &texture_width, &texture_height)
        || source.w <= 0.0f || source.h <= 0.0f) {
        return false;
    }
    const SDL_FRect original = source;
    const float left = std::clamp(source.x, 0.0f, texture_width);
    const float top = std::clamp(source.y, 0.0f, texture_height);
    const float right = std::clamp(source.x + source.w, 0.0f, texture_width);
    const float bottom = std::clamp(source.y + source.h, 0.0f, texture_height);
    if (right <= left || bottom <= top) {
        return false;
    }
    destination.x += (left - original.x) / original.w * destination.w;
    destination.y += (top - original.y) / original.h * destination.h;
    destination.w *= (right - left) / original.w;
    destination.h *= (bottom - top) / original.h;
    source = {left, top, right - left, bottom - top};
    return true;
}

class Game {
public:
    explicit Game(
        const std::filesystem::path& data,
        const std::optional<std::filesystem::path>& scenario)
        : scripts_(data / "SDT.PAK"), graphics_(data / "GRP.PAK"),
          backgrounds_(data / "bak.pak"), fonts_(data / "FNT.PAK"),
          bgm_archive_(data / "bgm.PAK"), se_archive_(data / "SE.PAK"),
          voice_archive_(data / "voice.pak"), movie_archive_(data / "mov.pak"),
          runtime_(scripts_),
          config_(th2::load_config(config_path_)),
          sdl_subsystem_(),
          font_(fonts_)
    {
        default_player_name_ =
            th2::load_default_player_name(data / "TOHEART2.EXE");
        if (config_.player_name.family.empty()) {
            config_.player_name = default_player_name_;
        }
        for (std::size_t i = 0; i < config_.game_flags.size(); ++i) {
            runtime_.set_game_flag(i, config_.game_flags[i]);
        }
        window_ = SDL_CreateWindow(
            "ToHeart2 XRATED", 1600, 1200, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        window_holder_.reset(window_);
        SDL_PropertiesID renderer_properties = SDL_CreateProperties();
        SDL_SetStringProperty(
            renderer_properties, SDL_PROP_RENDERER_CREATE_NAME_STRING,
            SDL_GPU_RENDERER);
        SDL_SetPointerProperty(
            renderer_properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
            window_);
        SDL_SetBooleanProperty(
            renderer_properties,
            SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
        renderer_ = window_
            ? SDL_CreateRendererWithProperties(renderer_properties) : nullptr;
        renderer_holder_.reset(renderer_);
        SDL_DestroyProperties(renderer_properties);
        if (!window_ || !renderer_) {
            throw std::runtime_error(SDL_GetError());
        }
        if (config_.fullscreen) {
            SDL_SetWindowFullscreen(window_, true);
        }
        upscaler_ = th2::create_upscaler(
            renderer_, TH2_ANIME4K_SHADER_DIR, config_.anime4k,
            &anime4k_available_);
        last_anime4k_wanted_ = config_.anime4k;
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
        omake_cg_background_ = try_load("t1000.tga");
        omake_cg_locked_ = try_load("t1100.tga");
        omake_music_background_ = try_load("t2000.tga");
        omake_music_selection_ = try_load("t2001.tga");
        omake_music_labels_ = try_load("t2010.tga");
        omake_music_title_ = try_load("t2020.tga");
        omake_music_artist_ = try_load("t2021.tga");
        omake_music_playing_ = try_load("t2100.tga");
        omake_replay_background_ = try_load("t3000.tga");
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
        if (scenario) {
            reset_play_state();
            initialize_scenario_flags();
            direct_scenario_ = true;
            runtime_.load_file(*scenario);
            ui_mode_ = UiMode::game;
            advance();
        } else {
            start_movie(3, 0, false);
        }
    }

    ~Game()
    {
        // All SDL resources are owned by members declared after
        // window_holder_/renderer_holder_, so they are destroyed before the
        // renderer/window and before SDL_Quit().  Only the config needs an
        // explicit teardown step.
        th2::save_config(config_path_, config_);
    }

    int run()
    {
        constexpr auto frame_duration = std::chrono::nanoseconds(
            1'000'000'000 / 120);
        auto next_frame = std::chrono::steady_clock::now();
        while (running_) {
            int window_width = 800;
            int window_height = 600;
            SDL_GetWindowSize(window_, &window_width, &window_height);
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running_ = false;
                    continue;
                }
                imgui_->process_event(event);
                convert_event_to_logical_coordinates(
                    event, window_width, window_height);
                if (config_.show_script_position
                    && imgui_->wants_mouse()
                    && (event.type == SDL_EVENT_MOUSE_MOTION
                        || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                        || event.type == SDL_EVENT_MOUSE_BUTTON_UP
                        || event.type == SDL_EVENT_MOUSE_WHEEL)) {
                    continue;
                }
                if (clock_state_) {
                    continue;
                }
                if (calendar_state_) {
                    const bool dismiss =
                        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                        || event.type == SDL_EVENT_KEY_DOWN;
                    const float frame = static_cast<float>(
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now()
                            - calendar_state_->started).count() * 60.0);
                    if (dismiss && !calendar_state_->dismissing
                        && frame >= 16.0f) {
                        calendar_state_->dismissing = true;
                        calendar_state_->started =
                            std::chrono::steady_clock::now();
                    }
                    continue;
                }
                if (movie_) {
                    const bool skip_key = event.type == SDL_EVENT_KEY_DOWN
                        && (event.key.key == SDLK_ESCAPE
                            || event.key.key == SDLK_SPACE
                            || event.key.key == SDLK_RETURN);
                    const bool skip_mouse = event.type
                        == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    const bool locked = (movie_mode_ == 0
                            && runtime_.game_flag(98) == 0)
                        || (movie_mode_ == 1
                            && runtime_.game_flag(80) == 0)
                        || (movie_mode_ == 2
                            && runtime_.game_flag(99) == 0);
                    if ((skip_key || skip_mouse) && !locked) {
                        const int skipped_mode = movie_mode_;
                        movie_.reset();
                        movie_bytes_.clear();
                        movie_mode_ = -1;
                        if (skipped_mode != 3) {
                            bgm_.stop();
                            bgm_track_ = -1;
                        }
                        if (movie_resume_script_) {
                            movie_resume_script_ = false;
                            advance();
                        } else if (skipped_mode == 3
                                   && runtime_.game_flag(98) != 0) {
                            start_movie(0, 0, false);
                        } else {
                            title_started_ = std::chrono::steady_clock::now();
                        }
                    }
                    continue;
                }
                if (config_open_ || name_input_open_) {
                    if (event.type == SDL_EVENT_KEY_DOWN
                        && event.key.key == SDLK_ESCAPE && config_open_) {
                        play_se(-1, 9107, false, 255);
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
                if (ui_mode_ == UiMode::cg_gallery) {
                    handle_cg_gallery_input(event);
                    continue;
                }
                if (ui_mode_ == UiMode::music_room) {
                    handle_music_room_input(event);
                    continue;
                }
                if (ui_mode_ == UiMode::replay_gallery) {
                    handle_replay_gallery_input(event);
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
                if (ui_mode_ == UiMode::map) {
                    handle_map_input(event);
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
                            manual_advance();
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
                        } else if (event.key.key == SDLK_F5
                                   && !replay_mode_) {
                            save_snapshot_ = capture_frame_pixels();
                            save(0);
                        } else if (event.key.key == SDLK_F7
                                   && !replay_mode_) {
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
                            manual_advance();
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
                                const float height =
                                    choice_height(choices_[i]);
                                if (mouse_y >= y && mouse_y < y + height) {
                                    choice_selected_ = i;
                                    manual_advance();
                                    break;
                                }
                                y += height;
                            }
                        } else {
                            manual_advance();
                        }
                    }
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (event.wheel.y > 0
                        && config_.wheel_opens_backlog) {
                        open_backlog();
                    } else if (event.wheel.y < 0) {
                        manual_advance();
                    }
                } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    update_sidebar_hover(event.motion.x, event.motion.y);
                    if (choosing_) {
                        const float mouse_y = event.motion.y;
                        float y = choice_y_start();
                        for (int i = 0;
                             i < static_cast<int>(choices_.size()); ++i) {
                            const float height =
                                choice_height(choices_[i]);
                            if (mouse_y >= y && mouse_y < y + height) {
                                choice_highlight_ = i;
                                break;
                            }
                            y += height;
                        }
                    }
                }
            }
            const bool control_held =
                (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            if (movie_) {
                movie_->set_speed(control_held ? 4.0 : 1.0);
            } else if (control_held && ui_mode_ == UiMode::title) {
                title_started_ -= std::chrono::milliseconds(25);
                if (title_exit_started_) {
                    *title_exit_started_ -= std::chrono::milliseconds(25);
                }
            } else if (control_held && ui_mode_ == UiMode::game) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= skip_next_time_) {
                    skip(true);
                    skip_next_time_ = now + std::chrono::milliseconds(40);
                }
            } else if (wake_time_
                       && std::chrono::steady_clock::now() >= *wake_time_) {
                wake_time_.reset();
                advance();
            }
            update_audio();
            update_movie();
            update_map();
            update_playback_modes();
            update_title();
            ensure_upscaler();
            int output_width = 800;
            int output_height = 600;
            SDL_GetRenderOutputSize(
                renderer_, &output_width, &output_height);
            const float scale_x = output_width / 800.0f;
            const float scale_y = output_height / 600.0f;
            const float framebuffer_scale = std::min(scale_x, scale_y);
            const float display_scale = SDL_GetWindowDisplayScale(window_);
            imgui_->new_frame(
                window_width, window_height,
                display_scale > 0.0f ? display_scale : 1.0f);
            font_.configure(
                config_.authentic_font, config_.font_family,
                config_.font_size, framebuffer_scale);
            draw_config();
            draw_name_input();
            draw();
            update_transition();
            update_background_fade();
            update_screen_flash();
            update_shake();
            update_background_scroll();
            update_character_animations();
            update_clock_calendar();
            update_sakura();
            next_frame += frame_duration;
            const auto now = std::chrono::steady_clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            } else if (now - next_frame > frame_duration * 4) {
                next_frame = now;
            }
        }
        return 0;
    }

private:
    static constexpr std::uint32_t save_version_ = 24;

    enum class AudioWaitKind {
        bgm,
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

    enum class CharacterAnimationKind {
        none,
        enter,
        leave,
        pose,
        locate,
        brightness,
        alpha,
    };

    struct CharacterAnimation {
        CharacterAnimationKind kind = CharacterAnimationKind::none;
        int type = 0;
        int frames = 1;
        int from_locate = 1;
        int to_locate = 1;
        int from_brightness = 128;
        int to_brightness = 128;
        int from_alpha = 256;
        int to_alpha = 256;
        bool blocking = false;
        Texture previous;
        std::chrono::steady_clock::time_point started;
    };

    struct OverlayState {
        std::string name;
        std::string archive;
        bool visible = true;
        int layer = 0;
        int tone_type = 0;
        int parameter = 0;
        int parameter_value = 0;
        int reverse = 0;
        int red = 128;
        int green = 128;
        int blue = 128;
        int destination_x = 0;
        int destination_y = 0;
        int destination_width = 0;
        int destination_height = 0;
        int source_x = 0;
        int source_y = 0;
        int source_width = 0;
        int source_height = 0;
        int zoom_center_x = 0;
        int zoom_center_y = 0;
        int zoom = 0;
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
        int frames = 1;
        int type = 1;
        bool resume_script = false;
        std::chrono::steady_clock::time_point started;
        std::uint64_t debug_id = 0;
        int last_dumped_frame = -1;
        bool debug_metadata_written = false;
    };

    struct BackgroundFade {
        std::array<float, 3> from{128.0f, 128.0f, 128.0f};
        std::array<float, 3> to{128.0f, 128.0f, 128.0f};
        std::chrono::steady_clock::time_point started;
        std::chrono::milliseconds duration;
    };

    struct ScreenFlash {
        int red = 255;
        int green = 255;
        int blue = 255;
        int fade_in_frames = 1;
        int fade_out_frames = 1;
        std::chrono::steady_clock::time_point started;
    };

    struct ShakeState {
        int type = 0;
        int pitch = 0;
        int frames = 0;
        int direction = 0;
        int swing = 256;
        int sampled_frame = 0;
        std::chrono::steady_clock::time_point started;
    };

    struct ShakeSample {
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        double angle = 0.0;
        bool text_only = false;
        bool includes_text = false;
    };

    struct BackgroundView {
        float x = 0.0f;
        float y = 0.0f;
        float width = 800.0f;
        float height = 600.0f;
    };

    enum class BackgroundKind : std::int32_t {
        background,
        visual,
        hcg,
    };

    struct BackgroundScroll {
        BackgroundView from;
        BackgroundView to;
        int frames = 1;
        int easing = 0;
        bool zoom = false;
        std::chrono::steady_clock::time_point started;
    };

    struct MapEvent {
        int character = 0;
        int position = 0;
        int type = 0;
        std::string script;
    };

    struct MapPosition {
        int field;
        int x;
        int y;
        int overlap;
    };

    struct MapSpritePart {
        SDL_FRect source;
        float x;
        float y;
    };

    struct MapSpriteStep {
        int frame;
        int ticks;
    };

    struct MapCharacter {
        Texture texture;
        std::vector<std::vector<MapSpritePart>> frames;
        std::vector<MapSpriteStep> steps;
    };

    struct ClockState {
        int target = 0;
        int start_minutes = 0;
        int target_minutes = 0;
        int travel_frames = 0;
        std::chrono::steady_clock::time_point started;
    };

    struct CalendarState {
        int month = 0;
        int day = 0;
        int weekday = 0;
        int holiday = -1;
        bool dismissing = false;
        std::chrono::steady_clock::time_point started;
    };

    struct SakuraPetal {
        bool active = false;
        int type = 0;
        float x = 0.0f;
        float y = 0.0f;
        float axis_x = 0.0f;
        float axis_y = 0.0f;
        std::uint32_t counter = 0;
    };

    struct SakuraState {
        std::array<SakuraPetal, 200> petals;
        int amount = 0;
        int target_amount = 0;
        float wind = 1.0f;
        int speed = 10;
        int tick = 0;
        int reset_frames = -1;
        bool no_reset = false;
        std::chrono::steady_clock::time_point updated;
    };

    static constexpr std::array<MapPosition, 22> map_positions_{{
        {0, 280, 340, 0}, {2, 488, 210, 1}, {2, 326, 356, 2},
        {3, 262, 304, 3}, {3, 464, 220, 4}, {4, 330, 216, 5},
        {4, 490, 294, 6}, {1, 356, 412, 7}, {1, 288, 210, 8},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 567, 326, 9},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 288, 210, 8},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 288, 210, 8},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 288, 210, 8},
        {1, 288, 210, 8},
    }};

    th2::Archive scripts_;
    th2::Archive graphics_;
    th2::Archive backgrounds_;
    th2::Archive fonts_;
    th2::Archive bgm_archive_;
    th2::Archive se_archive_;
    th2::Archive voice_archive_;
    th2::Archive movie_archive_;
    th2::ScriptRuntime runtime_;
    const std::filesystem::path config_path_{"toheart2-config.ini"};
    th2::GameConfig config_;

    // SDL subsystem lifetime.  window_/renderer_ holders are declared before
    // every other SDL-dependent member so that textures, audio streams, etc.
    // are destroyed while the renderer and SDL subsystems are still alive.
    SdlSubsystem sdl_subsystem_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    WindowPtr window_holder_;
    RendererPtr renderer_holder_;

    std::unique_ptr<th2::ImGuiLayer> imgui_;
    std::unique_ptr<th2::Upscaler> upscaler_;
    th2::GameFont font_;
    bool anime4k_available_ = false;
    bool last_anime4k_wanted_ = false;
    Texture background_;
    int bg_scene_ = -1;
    BackgroundKind background_kind_ = BackgroundKind::background;
    BackgroundView background_view_;
    std::optional<BackgroundScroll> background_scroll_;
    std::array<Texture, 32> overlays_{};
    std::array<Surface, 32> overlay_pixels_{};
    std::array<OverlayState, 32> overlay_states_{};
    th2::Characters characters_;
    std::array<CharacterTexture, 32> character_textures_{};
    std::array<CharacterAnimation, 32> character_animations_{};
    std::array<bool, 32> character_staged_{};
    std::array<bool, 32> character_pending_removal_{};
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
    int vi_event_voice_no_ = -1;
    int vi_event_voice_no_all_ = -1;
    std::vector<std::uint8_t> movie_bytes_;
    std::unique_ptr<th2::VideoPlayer> movie_;
    bool movie_resume_script_ = false;
    int movie_mode_ = -1;

    float bgm_gain(int volume) const
    {
        if (config_.bgm_muted) {
            return 0.0f;
        }
        return std::clamp(volume, 0, 255) / 255.0f
            * config_.bgm_volume / 256.0f;
    }

    float se_gain(int volume) const
    {
        if (config_.se_muted) {
            return 0.0f;
        }
        return std::clamp(volume, 0, 255) / 255.0f
            * config_.se_volume / 256.0f;
    }

    void ensure_upscaler()
    {
        if (last_anime4k_wanted_ == config_.anime4k) {
            return;
        }
        last_anime4k_wanted_ = config_.anime4k;
        upscaler_ = th2::create_upscaler(
            renderer_, TH2_ANIME4K_SHADER_DIR, config_.anime4k,
            &anime4k_available_);
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
        const auto index = voice_character_index(character);
        if (config_.voice_muted || config_.character_voice_muted[index]) {
            return 0.0f;
        }
        return std::clamp(volume, 0, 256) / 256.0f
            * config_.voice_volume / 256.0f
            * config_.character_voice_volume[index] / 256.0f;
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

    void start_movie(int mode, int number, bool resume_script)
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

    void update_movie()
    {
        if (!movie_) {
            return;
        }
        movie_->update();
        if (!movie_->finished()) {
            return;
        }
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
    Texture omake_cg_background_;
    Texture omake_cg_locked_;
    Texture omake_music_background_;
    Texture omake_music_selection_;
    Texture omake_music_labels_;
    Texture omake_music_title_;
    Texture omake_music_artist_;
    Texture omake_music_playing_;
    Texture omake_replay_background_;
    Texture map_frame_;
    Texture map_arrows_;
    Texture map_markers_;
    std::array<Texture, 5> map_fields_;
    std::vector<MapCharacter> map_characters_;
    Texture clock_background_;
    std::optional<MapCharacter> clock_animation_;
    std::optional<ClockState> clock_state_;
    Texture calendar_background_;
    Texture calendar_labels_;
    Texture calendar_days_;
    std::optional<CalendarState> calendar_state_;
    int skipped_month_ = 0;
    int skipped_day_ = 0;
    Texture sakura_large_;
    Texture sakura_small_;
    std::optional<SakuraState> sakura_;
    std::uint32_t sakura_random_ = 0x13579bdfu;
    Surface title_foreground_pixels_;
    Texture title_masked_;
    std::vector<std::uint8_t> title_mask_;
    int title_mask_width_ = 0;
    int title_mask_height_ = 0;
    th2::Message message_;
    bool message_ends_block_ = true;
    int tone_ = 0;
    int tone_back_ = -1;
    int tone_char_ = -1;
    int weather_ = 0;
    std::string background_tone_curve_;
    bool running_ = true;
    bool waiting_for_input_ = false;
    std::optional<std::chrono::steady_clock::time_point> wake_time_;
    std::optional<AudioWait> audio_wait_;
    std::optional<Transition> transition_;
    std::optional<BackgroundFade> background_fade_;
    std::optional<ScreenFlash> screen_flash_;
    std::optional<ShakeState> shake_;
    Texture shake_target_;
    std::array<float, 3> background_brightness_{128.0f, 128.0f, 128.0f};
    std::chrono::steady_clock::time_point skip_next_time_{};
    std::optional<std::chrono::steady_clock::time_point> auto_next_time_;
    std::string current_line_key_;
    std::uint64_t next_transition_debug_id_ = 1;
    bool direct_scenario_ = false;

    th2::AudioChannel& waited_audio_channel()
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
    enum class UiMode {
        title,
        cg_gallery,
        music_room,
        replay_gallery,
        game,
        system_menu,
        backlog,
        save,
        load,
        map,
    };
    UiMode ui_mode_ = UiMode::title;
    UiMode save_return_mode_ = UiMode::game;
    int title_highlight_ = 0;
    bool title_extras_ = false;
    bool title_extras_transition_from_ = false;
    std::optional<std::chrono::steady_clock::time_point>
        title_menu_transition_started_;
    int omake_highlight_ = 0;
    int omake_page_ = 0;
    int omake_music_playing_slot_ = -1;
    std::optional<int> omake_cg_view_;
    struct OmakeCgEntry {
        bool hcg = false;
        std::vector<int> variants;
    };
    std::vector<OmakeCgEntry> omake_cg_entries_;
    std::vector<Texture> omake_cg_thumbnails_;
    Texture omake_cg_full_;
    Texture omake_cg_previous_full_;
    int omake_cg_variant_ = 0;
    bool omake_cg_tall_scrolled_ = false;
    enum class OmakeCgPhase {
        viewing,
        opening,
        scrolling,
        changing,
        closing,
    };
    OmakeCgPhase omake_cg_phase_ = OmakeCgPhase::viewing;
    std::chrono::steady_clock::time_point omake_cg_phase_started_{};
    std::array<Texture, 9> omake_replay_thumbnails_;
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
    bool confirm_return_title_ = false;
    bool name_input_open_ = false;
    std::string name_error_;
    std::string load_error_;
    th2::PlayerName default_player_name_;
    std::array<char, 64> name_family_{};
    std::array<char, 64> name_given_{};
    std::array<char, 64> name_family_reading_{};
    std::array<char, 64> name_given_reading_{};
    std::array<char, 64> name_nickname_{};
    bool auto_mode_ = false;
    bool skip_mode_ = false;
    bool demo_mode_ = false;
    bool replay_mode_ = false;
    int demo_delay_frames_ = 0;
    std::vector<MapEvent> map_events_;
    int map_field_ = 1;
    int map_previous_field_ = 1;
    int map_hover_ = -1;
    int map_slide_ticks_ = 0;
    int map_fade_ticks_ = 0;
    int map_selected_ = -1;
    std::chrono::steady_clock::time_point map_tick_{};
    std::chrono::steady_clock::time_point map_started_{};

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
        if (replay_mode_) {
            return true;
        }
        const auto key = current_read_key();
        return !key.empty() && config_.read_lines.contains(key);
    }

    void mark_current_text_read()
    {
        if (replay_mode_) {
            return;
        }
        const auto key = current_read_key();
        if (!key.empty() && config_.read_lines.insert(key).second) {
            th2::save_config(config_path_, config_);
        }
    }

    void manual_advance()
    {
        auto_mode_ = false;
        skip_mode_ = false;
        auto_next_time_.reset();
        advance();
    }

    int map_sakura_type() const
    {
        const int month = runtime_.flag(0);
        const int day = runtime_.flag(1);
        if (month == 3) {
            return day <= 15 ? 4 : day <= 28 ? 2 : 0;
        }
        if (month == 4) {
            return day <= 15 ? 0 : day <= 27 ? 2 : 3;
        }
        return 3;
    }

    static std::uint16_t map_u16(
        std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        return static_cast<std::uint16_t>(bytes[offset])
            | static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
    }

    static std::uint32_t map_u32(
        std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        return static_cast<std::uint32_t>(bytes[offset])
            | static_cast<std::uint32_t>(bytes[offset + 1]) << 8
            | static_cast<std::uint32_t>(bytes[offset + 2]) << 16
            | static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
    }

    MapCharacter load_sprite_animation(const std::string& stem)
    {
        const auto* animation_entry = graphics_.find(stem + ".ani");
        if (!animation_entry) {
            throw std::runtime_error("map animation not found: " + stem);
        }
        const auto bytes = graphics_.read(*animation_entry);
        if (bytes.size() < 36 || map_u32(bytes, 0) != 0x53414e49) {
            throw std::runtime_error("invalid map animation: " + stem);
        }

        const auto frame_count = map_u32(bytes, 24);
        const auto sprite_count = map_u32(bytes, 28);
        std::size_t offset = 36;
        MapCharacter result;
        struct Operation {
            int code;
            int first;
            int second;
        };
        std::vector<Operation> operations;
        result.frames.resize(frame_count);
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            if (offset + 24 > bytes.size()) {
                throw std::runtime_error("truncated map animation frames");
            }
            const auto part_count = map_u32(bytes, offset + 20);
            offset += 24;
            for (std::size_t part = 0; part < part_count; ++part) {
                if (offset + 40 > bytes.size()) {
                    throw std::runtime_error(
                        "truncated map animation parts");
                }
                if (bytes[offset] != 0) {
                    result.frames[frame].push_back(MapSpritePart{
                        SDL_FRect{
                            static_cast<float>(map_u16(bytes, offset + 24)),
                            static_cast<float>(map_u16(bytes, offset + 26)),
                            static_cast<float>(map_u16(bytes, offset + 28)),
                            static_cast<float>(map_u16(bytes, offset + 30)),
                        },
                        static_cast<float>(
                            static_cast<std::int16_t>(
                                map_u16(bytes, offset + 32))),
                        static_cast<float>(
                            static_cast<std::int16_t>(
                                map_u16(bytes, offset + 34))),
                    });
                }
                offset += 40;
            }
        }

        for (std::size_t sprite = 0; sprite < sprite_count; ++sprite) {
            if (offset + 8 > bytes.size()) {
                throw std::runtime_error("truncated map animation sprites");
            }
            const auto operation_count = map_u32(bytes, offset + 4);
            offset += 8;
            for (std::size_t operation = 0;
                 operation <= operation_count; ++operation) {
                if (offset + 8 > bytes.size()) {
                    throw std::runtime_error(
                        "truncated map animation operations");
                }
                const auto code = map_u16(bytes, offset);
                if (sprite == 0) {
                    operations.push_back(Operation{
                        static_cast<int>(code),
                        static_cast<int>(map_u16(bytes, offset + 2)),
                        static_cast<int>(map_u16(bytes, offset + 4)),
                    });
                }
                offset += 8;
            }
        }
        struct Loop {
            std::size_t start;
            int remaining;
        };
        std::vector<Loop> loops;
        for (std::size_t pc = 0, guard = 0;
             pc < operations.size() && guard < 10000; ++guard) {
            const auto& operation = operations[pc];
            if (operation.code == 0) {
                break;
            }
            if (operation.code == 1) {
                result.steps.push_back(MapSpriteStep{
                    operation.first, operation.second + 1});
                ++pc;
            } else if (operation.code == 2) {
                loops.push_back(Loop{
                    pc + 1, operation.first + 1});
                ++pc;
            } else if (operation.code == 3 && !loops.empty()) {
                auto& loop = loops.back();
                if (--loop.remaining > 0) {
                    pc = loop.start;
                } else {
                    loops.pop_back();
                    ++pc;
                }
            } else {
                ++pc;
            }
        }
        if (result.steps.empty()) {
            result.steps.push_back({0, 1});
        }
        result.texture =
            load_texture(renderer_, graphics_, stem + ".tga");
        return result;
    }

    MapCharacter load_map_character(const MapEvent& event)
    {
        return load_sprite_animation(std::format(
            "mapc{:02d}{}", event.character, event.type));
    }

    static constexpr std::array<int, 20> clock_minutes_{
        8 * 60 + 43, 9 * 60 + 5, 9 * 60 + 25, 9 * 60 + 35,
        10 * 60, 10 * 60 + 20, 10 * 60 + 30, 10 * 60 + 55,
        11 * 60 + 15, 11 * 60 + 25, 11 * 60 + 50, 12 * 60 + 10,
        12 * 60 + 35, 13 * 60, 13 * 60 + 25, 13 * 60 + 45,
        13 * 60 + 55, 14 * 60 + 20, 14 * 60 + 40, 14 * 60 + 50,
    };

    int weekday(int month, int day) const
    {
        if (month == 3) return day % 7;
        if (month == 4) return (day + 3) % 7;
        if (month == 5) return (day + 5) % 7;
        return 0;
    }

    int calendar_holiday(int month, int day) const
    {
        struct Holiday {
            int month;
            int first;
            int last;
            int index;
        };
        static constexpr std::array holidays{
            Holiday{3, 12, 12, 0}, Holiday{3, 20, 20, 1},
            Holiday{3, 24, 24, 2}, Holiday{3, 25, 31, 3},
            Holiday{4, 1, 7, 3}, Holiday{4, 8, 8, 4},
            Holiday{4, 29, 29, 5}, Holiday{5, 3, 3, 6},
            Holiday{5, 4, 4, 7}, Holiday{5, 5, 5, 8},
        };
        for (const auto& holiday : holidays) {
            if (holiday.month == month
                && day >= holiday.first && day <= holiday.last) {
                return holiday.index;
            }
        }
        return -1;
    }

    void begin_clock(int requested)
    {
        const int current = std::clamp(runtime_.flag(7), 0, 19);
        int target = std::clamp(requested, 0, 19);
        if (current == target) {
            return;
        }
        if (weekday(runtime_.flag(0), runtime_.flag(1)) == 6) {
            target = std::min(target, 11);
        }
        if (!clock_background_) {
            clock_background_ =
                load_texture(renderer_, graphics_, "clock98.tga");
            clock_animation_ = load_sprite_animation("clock99");
        }
        const int start_minutes = clock_minutes_[current];
        const int target_minutes = clock_minutes_[target];
        clock_state_ = ClockState{
            target, start_minutes, target_minutes,
            std::max(0, (target_minutes - start_minutes + 5) / 6),
            std::chrono::steady_clock::now(),
        };
    }

    void begin_calendar(int month, int day)
    {
        if (skipped_month_ != 0) {
            runtime_.set_flag(0, skipped_month_);
            runtime_.set_flag(1, skipped_day_);
            runtime_.set_flag(2, -1);
            runtime_.set_flag(3, -1);
            runtime_.set_flag(4, 0);
            runtime_.set_flag(7, 0);
            skipped_month_ = 0;
            skipped_day_ = 0;
        }
        if (month < 0) {
            month = runtime_.flag(0);
            day = runtime_.flag(1);
        }
        calendar_background_ = load_texture(
            renderer_, graphics_, std::format("cal00{}.tga", month));
        if (!calendar_labels_) {
            calendar_labels_ =
                load_texture(renderer_, graphics_, "cal010.tga");
            calendar_days_ =
                load_texture(renderer_, graphics_, "cal011.tga");
        }
        background_.reset();
        characters_.clear();
        character_textures_ = {};
        calendar_state_ = CalendarState{
            month, day, weekday(month, day), calendar_holiday(month, day),
            false, std::chrono::steady_clock::now(),
        };
    }

    void update_clock_calendar()
    {
        if (clock_state_) {
            const float frame = static_cast<float>(
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - clock_state_->started).count() * 60.0);
            if (frame >= 32 + clock_state_->travel_frames) {
                runtime_.set_flag(7, clock_state_->target);
                clock_state_.reset();
                advance();
            }
        }
        if (calendar_state_ && calendar_state_->dismissing) {
            const float frame = static_cast<float>(
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - calendar_state_->started).count() * 60.0);
            if (frame >= 16.0f) {
                calendar_state_.reset();
                advance();
            }
        }
    }

    void draw_sprite_frame(
        const MapCharacter& animation, int frame, float x, float y)
    {
        if (frame < 0
            || static_cast<std::size_t>(frame) >= animation.frames.size()) {
            return;
        }
        for (const auto& part : animation.frames[frame]) {
            SDL_FRect destination{
                x + part.x, y + part.y, part.source.w, part.source.h};
            SDL_RenderTexture(
                renderer_, animation.texture.get(),
                &part.source, &destination);
        }
    }

    void draw_clock_calendar()
    {
        if (clock_state_) {
            const float frame = static_cast<float>(
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - clock_state_->started).count() * 60.0);
            float alpha = 1.0f;
            if (frame < 16.0f) {
                alpha = frame / 16.0f;
            } else if (frame > 16.0f + clock_state_->travel_frames) {
                alpha = 1.0f
                    - (frame - 16.0f - clock_state_->travel_frames) / 16.0f;
            }
            const int minutes = std::min(
                clock_state_->target_minutes,
                clock_state_->start_minutes
                    + std::max(0, static_cast<int>(frame) - 16) * 6);
            SDL_SetTextureAlphaModFloat(
                clock_background_.get(), std::clamp(alpha, 0.0f, 1.0f));
            SDL_RenderTexture(
                renderer_, clock_background_.get(), nullptr, nullptr);
            SDL_SetTextureAlphaModFloat(
                clock_animation_->texture.get(),
                std::clamp(alpha, 0.0f, 1.0f));
            draw_sprite_frame(
                *clock_animation_,
                (minutes / 60 % 12) * 10 + minutes % 60 / 6,
                400.0f, 300.0f);
            draw_sprite_frame(
                *clock_animation_, 120 + minutes % 60 * 2,
                400.0f, 300.0f);
            return;
        }
        if (!calendar_state_) {
            return;
        }
        const float frame = static_cast<float>(
            std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                - calendar_state_->started).count() * 60.0);
        const float alpha = calendar_state_->dismissing
            ? std::clamp(1.0f - frame / 16.0f, 0.0f, 1.0f)
            : std::clamp(frame / 16.0f, 0.0f, 1.0f);
        SDL_SetTextureAlphaModFloat(calendar_background_.get(), alpha);
        SDL_SetTextureAlphaModFloat(calendar_labels_.get(), alpha);
        SDL_SetTextureAlphaModFloat(calendar_days_.get(), alpha);
        SDL_RenderTexture(
            renderer_, calendar_background_.get(), nullptr, nullptr);

        static constexpr std::array<int, 7> weekday_type{2, 0, 0, 0, 0, 0, 1};
        int day_type = weekday_type[calendar_state_->weekday];
        if (calendar_state_->holiday == 1
            || calendar_state_->holiday >= 5) {
            day_type = 2;
        }
        const SDL_FRect day_source{
            static_cast<float>(day_type * 248),
            static_cast<float>((calendar_state_->day - 1) * 144),
            248.0f, 144.0f};
        const SDL_FRect day_destination{256.0f, 240.0f, 248.0f, 144.0f};
        SDL_RenderTexture(
            renderer_, calendar_days_.get(),
            &day_source, &day_destination);

        const SDL_FRect weekday_source{
            0.0f, static_cast<float>(calendar_state_->weekday * 32),
            168.0f, 32.0f};
        const SDL_FRect weekday_destination{88.0f, 352.0f, 168.0f, 32.0f};
        SDL_RenderTexture(
            renderer_, calendar_labels_.get(),
            &weekday_source, &weekday_destination);
        const SDL_FRect small_source{
            168.0f, static_cast<float>(calendar_state_->weekday * 34),
            34.0f, 34.0f};
        const SDL_FRect small_destination{504.0f, 347.0f, 34.0f, 34.0f};
        SDL_RenderTexture(
            renderer_, calendar_labels_.get(),
            &small_source, &small_destination);
        const int holiday = calendar_state_->holiday;
        const SDL_FRect holiday_source{
            static_cast<float>(202 + (holiday < 0 ? 0 : holiday / 3 * 164)),
            static_cast<float>((holiday < 0 ? 3 : holiday % 3) * 50),
            164.0f, 50.0f};
        const SDL_FRect holiday_destination{538.0f, 331.0f, 164.0f, 50.0f};
        SDL_RenderTexture(
            renderer_, calendar_labels_.get(),
            &holiday_source, &holiday_destination);
    }

    Texture load_sakura_texture(std::string_view name)
    {
        const auto* entry = graphics_.find(name);
        if (!entry) {
            throw std::runtime_error(
                "image not found: " + std::string(name));
        }
        Surface surface(th2::load_image(
            graphics_.read(*entry), entry->name));
        SDL_SetSurfaceColorKey(
            surface.get(), true,
            SDL_MapSurfaceRGB(surface.get(), 0, 0, 0));
        return texture_from_surface(surface.get());
    }

    void start_sakura(int amount, bool no_reset)
    {
        if (!sakura_large_) {
            sakura_large_ = load_sakura_texture("sakura.bmp");
            sakura_small_ = load_sakura_texture("sakura2.bmp");
        }
        if (!sakura_) {
            sakura_ = SakuraState{};
            sakura_->updated = std::chrono::steady_clock::now();
        }
        sakura_->target_amount = std::clamp(amount, 0, 200);
        sakura_->wind = 1.0f;
        sakura_->speed = 10;
        sakura_->reset_frames = -1;
        sakura_->no_reset = no_reset;
    }

    void stop_sakura(bool force)
    {
        if (sakura_ && (force || !sakura_->no_reset)
            && sakura_->reset_frames < 0) {
            sakura_->no_reset = false;
            sakura_->reset_frames = 0;
        }
    }

    int seasonal_background_scene(int scene) const
    {
        const int variant = scene % 10;
        int base = scene / 10;
        if (base >= 10000) {
            return scene;
        }
        if ((base >= 1 && base <= 4) || base == 78) {
            base = 1;
        } else if ((base >= 5 && base <= 8) || base == 79) {
            base = 5;
        } else if ((base >= 34 && base <= 37) || base == 80) {
            base = 34;
        } else if ((base >= 48 && base <= 51) || base == 81) {
            base = 48;
        } else {
            return scene;
        }

        int type = 3;
        const int month = runtime_.flag(0);
        const int day = runtime_.flag(1);
        if (month == 3) {
            type = day <= 15 ? 4 : day <= 28 ? 2 : 0;
        } else if (month == 4) {
            type = day <= 15 ? 0 : day <= 27 ? 1 : 3;
        }
        if (type == 4) {
            base = base == 1 ? 78 : base == 5 ? 79
                : base == 34 ? 80 : 81;
        } else {
            base += type;
        }
        return base * 10 + variant;
    }

    void update_background_sakura(int scene, bool background)
    {
        if (background) {
            const int base = scene / 10;
            if (base == 1 || base == 5 || base == 34 || base == 48) {
                start_sakura(32, false);
                return;
            }
        }
        stop_sakura(false);
    }

    std::uint32_t next_sakura_random()
    {
        sakura_random_ = sakura_random_ * 1103515245u + 12345u;
        return sakura_random_;
    }

    void spawn_sakura_petals()
    {
        if (!sakura_ || sakura_->reset_frames >= 0) {
            return;
        }
        while (sakura_->amount < sakura_->target_amount
               && sakura_->amount < static_cast<int>(sakura_->petals.size())
               && sakura_->tick % 5 == 0) {
            auto& petal = sakura_->petals[sakura_->amount++];
            petal.active = true;
            petal.type = static_cast<int>(next_sakura_random() % 6);
            petal.x = static_cast<float>(next_sakura_random() % 800);
            petal.y = -static_cast<float>(next_sakura_random() % 100);
            const int range = (6 - petal.type) * 100;
            petal.axis_x =
                (static_cast<float>(next_sakura_random() % range) / 100.0f
                 + 1.0f) / 2.0f;
            petal.axis_y =
                (static_cast<float>(next_sakura_random() % range) / 100.0f
                 + 1.0f) / 2.0f;
            petal.counter = next_sakura_random() % 256;
            break;
        }
    }

    void update_sakura()
    {
        if (!sakura_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        int steps = static_cast<int>(std::chrono::duration<double>(
            now - sakura_->updated).count() * 60.0);
        steps = std::clamp(steps, 0, 8);
        if (steps == 0) {
            return;
        }
        sakura_->updated += std::chrono::milliseconds(steps * 1000 / 60);
        for (int step = 0; step < steps; ++step) {
            ++sakura_->tick;
            if (sakura_->reset_frames >= 0) {
                ++sakura_->reset_frames;
            }
            spawn_sakura_petals();
            for (int i = 0; i < sakura_->amount; ++i) {
                auto& petal = sakura_->petals[i];
                if (!petal.active) {
                    continue;
                }
                petal.x += std::sin(
                    static_cast<float>(petal.counter % 256)
                    * 2.0f * std::numbers::pi_v<float> / 256.0f)
                    * petal.axis_x + sakura_->wind;
                petal.y += petal.axis_y;
                if (petal.y > 600.0f) {
                    if (sakura_->reset_frames >= 0) {
                        petal.active = false;
                    } else {
                        petal.x =
                            static_cast<float>(next_sakura_random() % 800);
                        petal.y =
                            -static_cast<float>(next_sakura_random() % 100);
                    }
                }
                if (petal.x >= 800.0f) {
                    if (sakura_->reset_frames >= 0) {
                        petal.active = false;
                        continue;
                    }
                    petal.x -= 830.0f;
                } else if (petal.x < -30.0f) {
                    if (sakura_->reset_frames >= 0) {
                        petal.active = false;
                        continue;
                    }
                    petal.x += 830.0f;
                }
                ++petal.counter;
            }
        }
        if (sakura_->reset_frames >= 16) {
            sakura_.reset();
        }
    }

    void draw_sakura()
    {
        if (!sakura_) {
            return;
        }
        const float alpha = sakura_->reset_frames < 0
            ? 1.0f
            : std::clamp(
                1.0f - sakura_->reset_frames / 16.0f, 0.0f, 1.0f);
        SDL_SetTextureAlphaModFloat(sakura_large_.get(), alpha);
        SDL_SetTextureAlphaModFloat(sakura_small_.get(), alpha);
        for (int i = 0; i < sakura_->amount; ++i) {
            const auto& petal = sakura_->petals[i];
            if (!petal.active) {
                continue;
            }
            SDL_FRect source;
            Texture* texture = nullptr;
            if (petal.type == 0) {
                const int frame = petal.counter / 2 % 23;
                source = {
                    static_cast<float>(40 * (frame % 10)),
                    static_cast<float>(static_cast<int>(
                        26.6666667f * (frame / 10))),
                    40.0f, 27.0f};
                texture = &sakura_small_;
            } else if (petal.type == 1) {
                const int frame = petal.counter / 2 % 20;
                source = {
                    static_cast<float>(static_cast<int>(
                        26.6666667f * (frame % 15))),
                    static_cast<float>(80 + 20 * (frame / 15)),
                    27.0f, 20.0f};
                texture = &sakura_small_;
            } else if (petal.type == 2) {
                const int frame = petal.counter / 2 % 17;
                source = {
                    static_cast<float>(static_cast<int>(
                        13.3333333f * (frame % 30))),
                    120.0f,
                    13.0f, 13.0f};
                texture = &sakura_small_;
            } else if (petal.type == 3) {
                const int frame = petal.counter / 2 % 23;
                source = {
                    static_cast<float>(30 * (frame % 10)),
                    static_cast<float>(20 * (frame / 10)), 30.0f, 20.0f};
                texture = &sakura_large_;
            } else if (petal.type == 4) {
                const int frame = petal.counter / 2 % 20;
                source = {
                    static_cast<float>(20 * (frame % 15)),
                    static_cast<float>(60 + 15 * (frame / 15)),
                    20.0f, 15.0f};
                texture = &sakura_large_;
            } else {
                const int frame = petal.counter / 2 % 17;
                source = {
                    static_cast<float>(10 * (frame % 30)), 90.0f,
                    10.0f, 10.0f};
                texture = &sakura_large_;
            }
            const SDL_FRect destination{
                petal.x, petal.y, source.w, source.h};
            SDL_RenderTexture(
                renderer_, texture->get(), &source, &destination);
        }
    }

    std::string map_field_name(int field) const
    {
        const int variant = field == 1 || field == 4
            ? map_sakura_type() : 0;
        return std::format("map1{}{}.tga", field, variant);
    }

    void begin_map()
    {
        if (std::ranges::none_of(
                map_events_, [](const MapEvent& event) {
                    return event.position == 0;
                })) {
            map_events_.push_back(MapEvent{});
        }
        map_frame_ = load_texture(renderer_, graphics_, "map000.tga");
        map_arrows_ = load_texture(renderer_, graphics_, "map010.tga");
        map_markers_ = load_texture(renderer_, graphics_, "map011.tga");
        std::array<bool, 5> present{};
        present[1] = true;
        for (const auto& event : map_events_) {
            if (event.position >= 0
                && event.position < static_cast<int>(map_positions_.size())) {
                present[map_positions_[event.position].field] = true;
            }
        }
        for (int field = 0; field < 5; ++field) {
            map_fields_[field].reset();
            if (present[field]) {
                map_fields_[field] =
                    load_texture(renderer_, graphics_, map_field_name(field));
            }
        }
        map_characters_.clear();
        map_characters_.resize(map_events_.size());
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            const auto& event = map_events_[i];
            if (event.character == 0) {
                continue;
            }
            const auto name = std::format(
                "mapc{:02d}{}.ani", event.character, event.type);
            if (graphics_.find(name)) {
                map_characters_[i] = load_map_character(event);
            }
        }
        map_field_ = 1;
        map_previous_field_ = 1;
        map_hover_ = -1;
        map_slide_ticks_ = 0;
        map_fade_ticks_ = 0;
        map_selected_ = -1;
        map_tick_ = std::chrono::steady_clock::now();
        map_started_ = map_tick_;
        play_bgm(10, true, 255);
        ui_mode_ = UiMode::map;
    }

    void finish_map_selection(int selected)
    {
        map_selected_ = selected;
        map_fade_ticks_ = 16;
        play_se(-1, 9014, false, 255);
    }

    void complete_map_selection()
    {
        const auto selected = map_events_.at(map_selected_);
        map_events_.clear();
        map_characters_.clear();
        map_frame_.reset();
        map_arrows_.reset();
        map_markers_.reset();
        for (auto& field : map_fields_) {
            field.reset();
        }
        runtime_.set_flag(4, 1);
        ui_mode_ = UiMode::game;
        if (selected.script.empty()) {
            runtime_.set_flag(3, 6);
            if (!load_scheduled_script()) {
                return_to_title();
                return;
            }
        } else {
            load_script(selected.script);
        }
        advance();
    }

    void load_script(std::string name)
    {
        runtime_.load(std::move(name));
        vi_event_voice_no_ = -1;
        vi_event_voice_no_all_ = -1;
    }

    bool load_scheduled_script()
    {
        constexpr std::size_t month_flag = 0;
        constexpr std::size_t day_flag = 1;
        constexpr std::size_t time_flag = 2;
        constexpr std::size_t event_next_flag = 3;
        constexpr std::size_t event_end_flag = 4;
        constexpr std::size_t calendar_skip_flag = 6;
        constexpr std::size_t clock_time_flag = 7;

        int month = runtime_.flag(month_flag);
        int day = runtime_.flag(day_flag);
        int time = runtime_.flag(time_flag);
        const int event_next = runtime_.flag(event_next_flag);
        bool show_calendar = false;

        if (runtime_.flag(event_end_flag) != 0) {
            time = event_next == -1 ? time + 1 : event_next;
            if (time >= 7) {
                map_events_.clear();
                if (skipped_month_ != 0) {
                    month = skipped_month_;
                    day = skipped_day_;
                    skipped_month_ = 0;
                    skipped_day_ = 0;
                } else {
                    ++day;
                }
                time = 0;
                runtime_.set_flag(clock_time_flag, 0);
                if (runtime_.flag(calendar_skip_flag) != 0) {
                    runtime_.set_flag(calendar_skip_flag, 0);
                } else {
                    show_calendar = true;
                }
                if ((month == 3 && day >= 32)
                    || (month == 4 && day >= 31)) {
                    ++month;
                    day = 1;
                }
            }
        }

        const int weekday = month == 3 ? day % 7
            : month == 4 ? (day + 3) % 7
            : (day + 5) % 7;
        const bool holiday =
            (month == 3 && (day == 20 || day >= 25))
            || (month == 4 && (day <= 7 || day == 29))
            || (month == 5 && (day == 3 || day == 4 || day == 5));
        if ((weekday == 0 || holiday) && time == 5) {
            time = 6;
        }

        if (time == 5) {
            runtime_.set_flag(month_flag, month);
            runtime_.set_flag(day_flag, day);
            runtime_.set_flag(time_flag, time);
            begin_map();
            return true;
        }
        map_events_.clear();

        static constexpr std::array periods{
            "MORNING", "INTERVAL", "LUNCH_BREAK", "SCHOOL_HOURS",
            "AFTER_SCHOOL", "", "NIGHT",
        };
        if (time < 0 || time >= static_cast<int>(periods.size())
            || periods[time][0] == '\0') {
            return false;
        }

        runtime_.set_flag(month_flag, month);
        runtime_.set_flag(day_flag, day);
        runtime_.set_flag(time_flag, time);
        runtime_.set_flag(event_end_flag, 0);
        runtime_.set_flag(event_next_flag, -1);
        load_script(std::format(
            "EV_{:02d}{:02d}{}.SDT", month, day, periods[time]));
        if (show_calendar) {
            begin_calendar(-1, -1);
        }
        return true;
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
            || transition_ || background_fade_ || screen_flash_
            || (shake_ && shake_->frames > 0)
            || background_scroll_ || character_animation_active()
            || clock_state_ || calendar_state_
            || wake_time_ || audio_wait_) {
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
        if ((!auto_mode_ && !demo_mode_)
            || !waiting_for_input_ || voice_playing()) {
            auto_next_time_.reset();
            return;
        }
        if (!auto_next_time_) {
            const int delay = demo_mode_
                ? std::max(0, demo_delay_frames_) * 1000 / 60
                : th2::auto_delay_ms(
                    config_, current_text_is_read(),
                    message_.has_hidden_segments(), message_ends_block_);
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
            return message_text_y()
                + static_cast<float>(display_lines(message_.visible()).size())
                    * 31.0f
                + 1.0f;
        }
        return 468.0f;
    }

    float choice_height(const Choice& choice) const
    {
        return std::max<std::size_t>(
            1, display_lines(choice.text).size()) * 31.0f;
    }

    std::vector<std::string> choice_lines(
        const Choice& choice, int index) const
    {
        auto lines = display_lines(choice.text);
        if (!lines.empty()) {
            lines.front() = std::format("{}. {}", index + 1, lines.front());
        }
        return lines;
    }

    float message_text_x() const
    {
        return config_.authentic_font ? 26.0f : 52.0f;
    }

    float message_text_y() const
    {
        return config_.authentic_font ? 36.0f : 72.0f;
    }

    void skip(bool force_unread = false)
    {
        if (clock_state_) {
            if (force_unread) {
                clock_state_->started -= std::chrono::milliseconds(250);
            }
            return;
        }
        if (calendar_state_) {
            if (!calendar_state_->dismissing) {
                calendar_state_->dismissing = true;
                calendar_state_->started = std::chrono::steady_clock::now();
            } else if (force_unread) {
                calendar_state_->started -= std::chrono::milliseconds(250);
            }
            return;
        }
        if (choosing_) {
            return;
        }
        if (transition_) {
            if (force_unread) {
                transition_->started = std::chrono::steady_clock::now()
                    - std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                            transition_->frames / 60.0));
            }
            return;
        }
        if (background_fade_) {
            if (force_unread) {
                background_fade_->started -= background_fade_->duration;
            }
            return;
        }
        if (screen_flash_) {
            if (force_unread) {
                const auto frames = screen_flash_->fade_in_frames
                    + screen_flash_->fade_out_frames;
                screen_flash_->started -= std::chrono::milliseconds(
                    frames * 1000 / 60);
            }
            return;
        }
        if (shake_ && shake_->frames > 0) {
            if (force_unread) {
                shake_->started -= std::chrono::milliseconds(
                    shake_->frames * 1000 / 60);
            }
            return;
        }
        if (background_scroll_) {
            if (force_unread) {
                background_scroll_->started -= std::chrono::milliseconds(
                    background_scroll_->frames * 1000 / 60);
            }
            return;
        }
        if (character_animation_active()) {
            if (force_unread) {
                for (auto& animation : character_animations_) {
                    if (animation.kind != CharacterAnimationKind::none) {
                        animation.started -= std::chrono::milliseconds(
                            animation.frames * 1000 / 60);
                    }
                }
            }
            return;
        }
        if (waiting_for_input_ && !force_unread && !config_.skip_unread
            && !current_text_is_read()) {
            skip_mode_ = false;
            return;
        }
        wake_time_.reset();
        if (audio_wait_) {
            waited_audio_channel().stop();
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

    Surface capture_frame_pixels(bool art_only = false)
    {
        (void)art_only;
        SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
        SDL_SetRenderTarget(renderer_, upscaler_->art_target());
        SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
        SDL_SetRenderTarget(renderer_, previous_target);
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
        auto previous_pixels = capture_frame_pixels(true);
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
            effective_frames,
            type,
            resume_script,
            std::chrono::steady_clock::now(),
            next_transition_debug_id_++,
            -1,
            false,
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
        const auto duration = std::chrono::duration<double>(
            static_cast<double>(transition_->frames) / 60.0);
        if (elapsed < duration) {
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

    void ensure_transition_target()
    {
        auto& transition = *transition_;
        if (transition.next_pixels) {
            return;
        }
        transition.next_pixels = capture_frame_pixels();
        if (transition.next_pixels->w != transition.previous_pixels->w
            || transition.next_pixels->h != transition.previous_pixels->h) {
            throw std::runtime_error("transition frame size changed");
        }
        transition.composite.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
            transition.next_pixels->w, transition.next_pixels->h));
        if (!transition.composite) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    void draw_pixel_transition(float progress)
    {
        ensure_transition_target();
        auto& transition = *transition_;
        const int width = transition.next_pixels->w;
        const int height = transition.next_pixels->h;
        const int rate = std::clamp(static_cast<int>(progress * 256.0f), 0, 256);
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * height * 4);
        const auto* previous =
            static_cast<const std::uint8_t*>(transition.previous_pixels->pixels);
        const auto* next =
            static_cast<const std::uint8_t*>(transition.next_pixels->pixels);

        auto source_alpha = [&](int x, int y) {
            switch (transition.type) {
            case 2: {
                const int edge = (height + 255) * (256 - rate) / 256;
                return std::clamp((y - edge + 255) * 256 / 255, 0, 256);
            }
            case 3: {
                const int edge = (height + 255) * rate / 256;
                return std::clamp((edge - y) * 256 / 255, 0, 256);
            }
            case 4: {
                const int edge = (width + 255) * (256 - rate) / 256;
                return std::clamp((x - edge + 255) * 256 / 255, 0, 256);
            }
            case 5: {
                const int edge = (width + 255) * rate / 256;
                return std::clamp((edge - x) * 256 / 255, 0, 256);
            }
            case 6: {
                const int edge = (width / 2 + 127) * (256 - rate) / 256;
                return std::clamp(
                    (width / 2 - std::abs(x - width / 2) - edge + 127)
                        * 256 / 127,
                    0, 256);
            }
            case 7: {
                const int edge = (width / 2 + 127) * rate / 256;
                return std::clamp(
                    (std::abs(x - width / 2) - width / 2 + edge)
                        * 256 / 127,
                    0, 256);
            }
            case 8:
            case 9:
            case 10: {
                const int shift = transition.type - 8;
                const int mask = 0x3f >> shift;
                const int half = 32 >> shift;
                const int extent = (96 >> shift) * rate / 256 - half;
                const bool inside = std::abs((x & mask) - half)
                    < std::abs((y & mask) - half) + extent;
                return inside ? 16 + rate * rate / 512 : 0;
            }
            case 21: {
                std::uint32_t value = static_cast<std::uint32_t>(
                    x + y * width + transition.debug_id * 0x9e3779b9U);
                value ^= value >> 16;
                value *= 0x7feb352dU;
                value ^= value >> 15;
                return static_cast<int>(value & 0xff) < rate ? 256 : 0;
            }
            default:
                return rate;
            }
        };

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int alpha = source_alpha(x, y);
                const auto old_offset =
                    static_cast<std::size_t>(y) * transition.previous_pixels->pitch
                    + static_cast<std::size_t>(x) * 4;
                const auto new_offset =
                    static_cast<std::size_t>(y) * transition.next_pixels->pitch
                    + static_cast<std::size_t>(x) * 4;
                const auto output_offset =
                    (static_cast<std::size_t>(y) * width + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    pixels[output_offset + channel] =
                        static_cast<std::uint8_t>(
                            (previous[old_offset + channel] * (256 - alpha)
                             + next[new_offset + channel] * alpha)
                            / 256);
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

    void draw_geometric_transition(float progress)
    {
        ensure_transition_target();
        auto& transition = *transition_;
        if (!SDL_UpdateTexture(
                transition.composite.get(), nullptr,
                transition.next_pixels->pixels,
                transition.next_pixels->pitch)) {
            throw std::runtime_error(SDL_GetError());
        }
        const int rate = std::clamp(static_cast<int>(progress * 256.0f), 0, 256);
        auto draw_old = [&](SDL_FRect destination, float alpha = 1.0f) {
            SDL_SetTextureAlphaModFloat(transition.previous.get(), alpha);
            SDL_RenderTexture(
                renderer_, transition.previous.get(), nullptr, &destination);
        };
        const SDL_FRect full{0.0f, 0.0f, 800.0f, 600.0f};

        switch (transition.type) {
        case 11: {
            int zoom = 256 - rate * rate / 256;
            const float scale = (zoom + 256.0f) / 256.0f;
            SDL_FRect rectangle{
                400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
                800.0f * scale, 600.0f * scale};
            SDL_RenderTexture(renderer_, transition.previous.get(), nullptr, nullptr);
            SDL_SetTextureAlphaModFloat(
                transition.composite.get(), (128.0f - zoom / 2.0f) / 128.0f);
            SDL_RenderTexture(
                renderer_, transition.composite.get(), nullptr, &rectangle);
            break;
        }
        case 12: {
            const int inverse = 256 - rate;
            const int eased = 256 - inverse * inverse / 256;
            const float scale = (eased * 2.0f + 256.0f) / 256.0f;
            SDL_FRect rectangle{
                400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
                800.0f * scale, 600.0f * scale};
            draw_old(rectangle);
            SDL_SetTextureAlphaModFloat(
                transition.composite.get(), eased / 256.0f);
            SDL_RenderTexture(
                renderer_, transition.composite.get(), nullptr, nullptr);
            break;
        }
        case 13: {
            const int inverse = 256 - rate;
            const int eased = inverse * inverse / 256;
            const float scale = 0.5f + eased / 512.0f;
            SDL_FRect rectangle{
                400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
                800.0f * scale, 600.0f * scale};
            draw_old(rectangle, eased / 256.0f);
            break;
        }
        case 14: {
            const int inverse = 256 - rate;
            const int zoom = -(inverse * inverse / 256) / 2;
            const float scale = (zoom + 256.0f) / 256.0f;
            SDL_FRect rectangle{
                400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
                800.0f * scale, 600.0f * scale};
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
            SDL_RenderClear(renderer_);
            SDL_SetTextureAlphaModFloat(
                transition.composite.get(), (32.0f - inverse / 8.0f) / 32.0f);
            SDL_RenderTexture(
                renderer_, transition.composite.get(), nullptr, &rectangle);
            break;
        }
        case 15: {
            const float x = 800.0f * rate / 256.0f;
            SDL_FRect left{-x, 0.0f, 800.0f, 600.0f};
            SDL_FRect right{x, 0.0f, 800.0f, 600.0f};
            draw_old(left, 0.5f);
            draw_old(right, 0.5f);
            break;
        }
        case 16:
        case 17:
        case 18:
        case 19: {
            const int inverse = 256 - rate;
            const float x = 800.0f
                - 800.0f * inverse * inverse / (256.0f * 256.0f);
            const float y = 600.0f
                - 600.0f * inverse * inverse / (256.0f * 256.0f);
            SDL_FRect rectangle = full;
            if (transition.type == 16) rectangle.y = y;
            if (transition.type == 17) rectangle.y = -y;
            if (transition.type == 18) rectangle.x = -x;
            if (transition.type == 19) rectangle.x = x;
            draw_old(rectangle, 1.0f - rate / 256.0f);
            break;
        }
        case 20: {
            const int amplitude = std::min(255, rate);
            for (int y = 0; y < 600; ++y) {
                const float offset = amplitude
                    * std::sin((y * 480.0f / 600.0f + rate * 2.0f)
                               * std::numbers::pi_v<float> / 128.0f)
                    / 10.0f;
                const SDL_FRect source{
                    std::max(0.0f, offset), static_cast<float>(y),
                    800.0f - std::abs(offset), 1.0f};
                SDL_FRect destination{
                    std::max(0.0f, -offset), static_cast<float>(y),
                    source.w, 1.0f};
                SDL_RenderTexture(
                    renderer_, transition.previous.get(), &source, &destination);
            }
            break;
        }
        case 22: {
            for (int y = 0; y < 600; ++y) {
                const float offset = rate
                    * std::sin((y + rate * 2.0f)
                               * std::numbers::pi_v<float> / 128.0f)
                    / 10.0f;
                const SDL_FRect source{
                    0.0f, static_cast<float>(
                        std::clamp(y + static_cast<int>(offset), 0, 599)),
                    800.0f, 1.0f};
                const SDL_FRect destination{
                    0.0f, static_cast<float>(y), 800.0f, 1.0f};
                SDL_SetTextureAlphaModFloat(
                    transition.previous.get(), 32.0f / 256.0f);
                SDL_RenderTexture(
                    renderer_, transition.previous.get(), &source, &destination);
            }
            break;
        }
        case 23: {
            int tv = 150 - 150 * rate / 256;
            tv = 150 - tv * tv * tv / (150 * 150);
            const float h = std::max(0.0f, 600.0f - 600.0f * tv / 120.0f);
            SDL_FRect rectangle{0.0f, (600.0f - h) / 2.0f, 800.0f, h};
            draw_old(rectangle);
            break;
        }
        case 24: {
            const float roll = 128.0f * rate / 256.0f;
            const float angle = roll * 360.0f / 256.0f;
            const float scale = (roll + 256.0f) / 256.0f;
            SDL_FRect rectangle{
                400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
                800.0f * scale, 600.0f * scale};
            SDL_SetTextureAlphaModFloat(
                transition.previous.get(),
                std::clamp((128.0f - roll) / 128.0f, 0.0f, 1.0f));
            SDL_RenderTextureRotated(
                renderer_, transition.previous.get(), nullptr, &rectangle,
                angle, nullptr, SDL_FLIP_NONE);
            break;
        }
        default:
            break;
        }
    }

    void dump_transition_frame(float progress)
    {
        if (!config_.dump_transition_frames || !transition_) {
            return;
        }
        const int frame_count = transition_->frames;
        const int frame = std::clamp(
            static_cast<int>(progress * frame_count), 0, frame_count);
        if (frame == transition_->last_dumped_frame) {
            return;
        }
        transition_->last_dumped_frame = frame;

        const auto directory = std::filesystem::path("debug/transitions")
            / std::format("{:06}_{}_{}", transition_->debug_id,
                          runtime_.script_name(), runtime_.vm_pc());
        std::filesystem::create_directories(directory);
        auto surface = capture_frame_pixels();
        const auto path = directory / std::format("frame_{:04}.bmp", frame);
        if (!SDL_SaveBMP(surface.get(), path.string().c_str())) {
            std::cerr << "transition dump: " << SDL_GetError() << '\n';
        }
        if (!transition_->debug_metadata_written) {
            transition_->debug_metadata_written = true;
            std::ofstream metadata(directory / "transition.txt");
            metadata << "script=" << runtime_.script_name() << '\n'
                     << "vm_pc=" << runtime_.vm_pc() << '\n'
                     << "type=" << transition_->type << '\n'
                     << "vague=" << transition_->vague << '\n'
                     << "frames=" << transition_->frames << '\n'
                     << "mask_width=" << transition_->mask_width << '\n'
                     << "mask_height=" << transition_->mask_height << '\n';
        }
    }

    void draw_active_transition()
    {
        if (!transition_) {
            return;
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - transition_->started);
        const float progress = std::clamp(
            static_cast<float>(
                elapsed.count() * 60.0 / transition_->frames),
            0.0f, 1.0f);
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
        } else if ((transition_->type >= 2 && transition_->type <= 10)
                   || transition_->type == 21) {
            draw_pixel_transition(progress);
        } else if (transition_->type >= 11
                   && transition_->type <= 24) {
            draw_geometric_transition(progress);
        } else {
            SDL_SetTextureAlphaModFloat(
                transition_->previous.get(), 1.0f - progress);
            SDL_RenderTexture(
                renderer_, transition_->previous.get(), nullptr, nullptr);
        }
        dump_transition_frame(progress);
    }

    void draw_script_position()
    {
        if (!config_.show_script_position || ui_mode_ != UiMode::game) {
            return;
        }
        const auto position = std::format(
            "{}:{}  line={}", runtime_.script_name(), runtime_.vm_pc(),
            current_line_key_.empty() ? "-" : current_line_key_);
        ImGui::SetNextWindowPos(
            ImVec2(8.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin(
            "Script position", nullptr,
            ImGuiWindowFlags_NoDecoration
                | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(position.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            SDL_SetClipboardText(position.c_str());
        }
        ImGui::End();
    }

    void begin_background_fade(int red, int green, int blue, int frames)
    {
        const int effective_frames = frames > 0 ? frames : 30;
        background_fade_ = BackgroundFade{
            background_brightness_,
            {
                static_cast<float>(std::clamp(red, 0, 256)),
                static_cast<float>(std::clamp(green, 0, 256)),
                static_cast<float>(std::clamp(blue, 0, 256)),
            },
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
        for (std::size_t i = 0; i < background_brightness_.size(); ++i) {
            background_brightness_[i] = background_fade_->from[i]
                + (background_fade_->to[i] - background_fade_->from[i])
                    * progress;
        }
        if (progress >= 1.0f) {
            background_brightness_ = background_fade_->to;
            background_fade_.reset();
            advance();
        }
    }

    void update_screen_flash()
    {
        if (!screen_flash_) {
            return;
        }
        const int total_frames =
            screen_flash_->fade_in_frames + screen_flash_->fade_out_frames;
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - screen_flash_->started).count();
        if (elapsed * 60.0 >= total_frames) {
            screen_flash_.reset();
            advance();
        }
    }

    float screen_flash_alpha() const
    {
        if (!screen_flash_) {
            return 0.0f;
        }
        const float frame = static_cast<float>(
            std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                - screen_flash_->started).count() * 60.0);
        if (frame < screen_flash_->fade_in_frames) {
            return std::clamp(
                frame / screen_flash_->fade_in_frames, 0.0f, 1.0f);
        }
        return std::clamp(
            1.0f - (frame - screen_flash_->fade_in_frames)
                / screen_flash_->fade_out_frames,
            0.0f, 1.0f);
    }

    void update_shake()
    {
        if (!shake_ || shake_->frames == 0) {
            return;
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - shake_->started).count();
        if (elapsed * 60.0 >= shake_->frames) {
            shake_.reset();
            advance();
        }
    }

    ShakeSample shake_sample()
    {
        ShakeSample result;
        if (!shake_) {
            return result;
        }
        const float frame = static_cast<float>(
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - shake_->started).count()
            * 60.0);
        const int frame_index = std::max(1, static_cast<int>(frame) + 1);
        const float decay = shake_->frames > 0
            ? std::clamp(
                1.0f - static_cast<float>(frame_index) / shake_->frames,
                0.0f, 1.0f)
            : 1.0f;
        const int phase = frame_index * shake_->swing / 8;
        const float cosine = std::cos(
            static_cast<float>(phase % 256)
            * 2.0f * std::numbers::pi_v<float> / 256.0f);
        float amount = static_cast<float>(shake_->pitch);
        if (shake_->type == 0 || shake_->type == 3
            || shake_->type == 6 || shake_->type == 15
            || shake_->type == 16) {
            amount *= cosine;
            amount *= decay;
        } else if (shake_->type == 1 || shake_->type == 4
                   || shake_->type == 7) {
            amount *= (frame_index & 1) ? 1.0f : -1.0f;
        } else if (shake_->type == 9 || shake_->type == 10
                   || shake_->type == 11) {
            while (shake_->sampled_frame < frame_index) {
                int direction = std::rand() % 8;
                while (direction == shake_->direction) {
                    direction = std::rand() % 8;
                }
                shake_->direction = direction;
                ++shake_->sampled_frame;
            }
        } else if (shake_->type == 2) {
            const float root = std::sqrt(std::max(0, shake_->pitch));
            const float cycle = std::fmod(
                frame_index * root * 2.0f / std::max(1, shake_->frames),
                std::max(1.0f, root));
            result.scale = 1.0f + cycle * cycle / 256.0f;
        } else if (shake_->type == 12) {
            const float inverse = std::clamp(
                1.0f - static_cast<float>(frame_index)
                    / std::max(1, shake_->frames),
                0.0f, 1.0f);
            const float eased = 1.0f - inverse * inverse;
            float turn = std::fmod(eased * shake_->pitch / 2.0f, 256.0f);
            if ((shake_->direction & 1) == 0) {
                turn = 256.0f - turn;
            }
            result.angle = turn * 360.0 / 256.0;
        } else if (shake_->type == 13) {
            const float turn = -cosine * shake_->pitch * decay;
            result.angle = turn * 360.0 / 256.0;
        } else if (shake_->type == 14) {
            static constexpr std::array<int, 4> steps{-1, 0, 1, 0};
            result.angle = steps[frame_index & 3]
                * shake_->pitch * 360.0 / 256.0;
        }

        if (result.x == 0.0f && result.y == 0.0f
            && result.scale == 1.0f && result.angle == 0.0) {
            const bool left = shake_->direction == 1
                || shake_->direction == 2 || shake_->direction == 3;
            const bool right = shake_->direction == 5
                || shake_->direction == 6 || shake_->direction == 7;
            const bool up = shake_->direction == 3
                || shake_->direction == 4 || shake_->direction == 5;
            const bool down = shake_->direction == 0
                || shake_->direction == 1 || shake_->direction == 7;
            result.x = left ? -amount : right ? amount : 0.0f;
            result.y = up ? -amount : down ? amount : 0.0f;
        }
        result.text_only = shake_->type == 3 || shake_->type == 4
            || shake_->type == 10;
        result.includes_text = shake_->type == 6 || shake_->type == 7
            || shake_->type == 11 || shake_->type == 16;
        return result;
    }

    BackgroundView current_background_view() const
    {
        if (!background_scroll_) {
            return background_view_;
        }
        const float raw = std::clamp(
            static_cast<float>(std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                - background_scroll_->started).count()
                * 60.0 / background_scroll_->frames),
            0.0f, 1.0f);
        const float progress = background_scroll_->easing == 1
            ? raw * raw
            : background_scroll_->easing == 2
                ? 1.0f - (1.0f - raw) * (1.0f - raw)
                : raw;
        if (background_scroll_->zoom) {
            const auto zoom_axis = [progress](
                float from_position, float from_size,
                float to_position, float to_size) {
                const float inverse_size =
                    (1.0f - progress) / from_size + progress / to_size;
                const float size = 1.0f / inverse_size;
                const float position =
                    ((1.0f - progress) * from_position / from_size
                     + progress * to_position / to_size)
                    * size;
                return std::pair{position, size};
            };
            const auto [x, width] = zoom_axis(
                background_scroll_->from.x,
                background_scroll_->from.width,
                background_scroll_->to.x,
                background_scroll_->to.width);
            const auto [y, height] = zoom_axis(
                background_scroll_->from.y,
                background_scroll_->from.height,
                background_scroll_->to.y,
                background_scroll_->to.height);
            return {x, y, width, height};
        }
        const auto mix = [progress](float from, float to) {
            return from + (to - from) * progress;
        };
        return {
            mix(background_scroll_->from.x, background_scroll_->to.x),
            mix(background_scroll_->from.y, background_scroll_->to.y),
            mix(background_scroll_->from.width, background_scroll_->to.width),
            mix(background_scroll_->from.height, background_scroll_->to.height),
        };
    }

    void update_background_scroll()
    {
        if (!background_scroll_) {
            return;
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now()
            - background_scroll_->started).count();
        if (elapsed * 60.0 >= background_scroll_->frames) {
            background_view_ = background_scroll_->to;
            background_scroll_.reset();
            advance();
        }
    }

    void begin_background_scroll(
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

    void load_character_texture(const th2::CharacterState& character)
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

    void reload_character_textures()
    {
        character_textures_ = {};
        for (const auto& character : characters_.ordered()) {
            load_character_texture(character);
        }
    }

    void apply_staged_characters()
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

    void clear_characters()
    {
        characters_.clear();
        character_textures_ = {};
        character_animations_ = {};
        character_staged_ = {};
        character_pending_removal_ = {};
    }

    std::size_t character_index(int character_number) const
    {
        if (character_number < 0
            || static_cast<std::size_t>(character_number)
                >= character_textures_.size()) {
            throw std::out_of_range(std::format(
                "invalid character number: {}", character_number));
        }
        return static_cast<std::size_t>(character_number);
    }

    static std::vector<ToneCurveSpec> effect_tone_curves(
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

    static std::string base_tone_curve(int tone)
    {
        switch (tone % 4) {
        case 1: return "evening.amp";
        case 2: return "night.amp";
        case 3: return "indoor.amp";
        default: return {};
        }
    }

    std::vector<ToneCurveSpec> background_tone_curves() const
    {
        const int tone = tone_back_ < 0 ? tone_ : tone_back_;
        return effect_tone_curves(tone, false);
    }

    std::vector<ToneCurveSpec> character_tone_curves() const
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

    static int character_effect_frames(int frames)
    {
        return frames < 0 ? 15 : std::max(frames, 0);
    }

    bool character_animation_active() const
    {
        return std::ranges::any_of(
            character_animations_, [](const CharacterAnimation& animation) {
                return animation.kind != CharacterAnimationKind::none;
            });
    }

    void start_character_animation(
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

    void update_character_animations()
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

    void set_character(const th2::Event& event)
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

    void play_se(int channel, int sound, bool loop, int volume, int fade = 0,
                 bool wait_for_completion = false)
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

    void sync_game_flags()
    {
        const auto flags = runtime_.all_game_flags();
        if (std::ranges::equal(flags, config_.game_flags)) {
            return;
        }
        std::ranges::copy(flags, config_.game_flags.begin());
        th2::save_config(config_path_, config_);
    }

    void play_bgm(int music, bool loop, int volume)
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

    void play_voice(const th2::Event& event)
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
        voice_channel.play(
            load_audio(voice_archive_, name), loop,
            voice_gain(volume, character));
        voice_sound_[channel] = voice;
        voice_character_[channel] = character;
        voice_scenario_[channel] = scenario;
        voice_volume_[channel] = volume;
        voice_loop_[channel] = loop;
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

    void set_background(const th2::Event& event, bool keep_characters)
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

    void set_cg(
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
            ? config_.unlocked_visual_cgs : config_.unlocked_h_cgs;
        if (unlocked.emplace(visual).second) {
            th2::save_config(config_path_, config_);
        }
        const bool keep_characters = number(event, 4) > 0;
        if (keep_characters) {
            apply_staged_characters();
            reload_character_textures();
        } else {
            clear_characters();
        }
    }

    void restore_background()
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

    std::optional<std::size_t> overlay_index(int requested) const
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

    void load_overlay(
        std::size_t slot, std::string name, std::string archive,
        int tone_type = 0)
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

    void apply_overlay_brightness(std::size_t slot)
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

    bool handle(const th2::Event& event)
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
            config_.unlocked_replays.emplace(number(event, 0));
            th2::save_config(config_path_, config_);
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
                text(event, 0), config_.player_name));
            message_visible_ = true;
            current_line_key_ = runtime_.script_name() + ':'
                + std::to_string(runtime_.vm_pc());
            message_ends_block_ = number(event, 1) == 2;
            waiting_for_input_ = true;
            auto_next_time_.reset();
        } else if (name == "AddMessage2") {
            message_.append(th2::substitute_player_name(
                text(event, 0), config_.player_name));
            message_visible_ = true;
            current_line_key_ = runtime_.script_name() + ':'
                + std::to_string(runtime_.vm_pc());
            message_ends_block_ = number(event, 1) == 2;
            waiting_for_input_ = true;
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
                    text(event, 0), config_.player_name)),
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
            load_script(text(event, 0));
        } else {
            return false;
        }
        return true;
    }

    std::filesystem::path dump_engine_error(
        const th2::ScriptStep& step, std::string_view error)
    {
        std::filesystem::create_directories("logs");
        const auto now = std::chrono::system_clock::now();
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        const auto path = std::filesystem::path("logs")
            / std::format("engine-error-{}.log", stamp);
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

    std::filesystem::path dump_runtime_error(std::string_view error)
    {
        std::filesystem::create_directories("logs");
        const auto now = std::chrono::system_clock::now();
        const auto stamp = std::chrono::duration_cast<
            std::chrono::milliseconds>(now.time_since_epoch()).count();
        const auto path = std::filesystem::path("logs")
            / std::format("engine-error-{}.log", stamp);
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

    void advance(bool skipping = false)
    {
        if (wake_time_ || audio_wait_ || transition_ || background_fade_
            || screen_flash_
            || (shake_ && shake_->frames > 0)
            || background_scroll_ || character_animation_active()
            || clock_state_ || calendar_state_
            || movie_) {
            return;
        }
        if (waiting_for_input_) {
            mark_current_text_read();
        }
        if (waiting_for_input_ && message_.reveal_next()) {
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
    }

    void save(int slot)
    {
        if (replay_mode_ || wake_time_ || audio_wait_ || transition_
            || background_fade_ || screen_flash_
            || (shake_ && shake_->frames > 0)
            || background_scroll_ || character_animation_active()
            || clock_state_ || calendar_state_ || movie_) {
            return;
        }
        std::filesystem::create_directories("save");
        const auto path = save_path(slot);
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            return;
        }
        save_body(file);
        file.close();
        save_preview(slot);
    }

    bool load(int slot)
    {
        const auto path = save_path(slot);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        return load_body(file);
    }

    std::filesystem::path save_path(int slot) const
    {
        return std::filesystem::path("save")
            / std::format("save_{:02d}.sav", slot);
    }

    std::filesystem::path thumbnail_path(int slot) const
    {
        return std::filesystem::path("save")
            / std::format("save_{:02d}.bmp", slot);
    }

    std::filesystem::path metadata_path(int slot) const
    {
        return std::filesystem::path("save")
            / std::format("save_{:02d}.meta", slot);
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

    bool load_body(std::istream& in)
    {
        const auto version = read_u32(in);
        if (version != save_version_) {
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
            backlog_.push_back({std::move(history_text)});
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
        play_se(-1, 9104, false, 255);
        save_snapshot_ = capture_frame_pixels();
        begin_transition(1, 12, 128, false);
        ui_mode_ = UiMode::system_menu;
        menu_highlight_ = 4;
    }

    void reset_play_state()
    {
        bgm_.stop();
        bgm_track_ = -1;
        bgm_loop_ = false;
        bgm_volume_ = 255;
        for (auto& channel : transient_se_) {
            channel.stop();
        }
        transient_se_volume_ = {};
        for (auto& channel : se_channels_) {
            channel.stop();
        }
        se_sound_.fill(-1);
        se_loop_ = {};
        se_volume_ = {};
        for (auto& channel : voice_channels_) {
            channel.stop();
        }
        voice_sound_.fill(-1);
        voice_character_ = {};
        voice_scenario_ = {};
        voice_volume_ = {};
        voice_loop_ = {};
        vi_event_voice_no_ = -1;
        vi_event_voice_no_all_ = -1;
        movie_.reset();
        movie_bytes_.clear();
        movie_resume_script_ = false;
        movie_mode_ = -1;

        background_.reset();
        bg_scene_ = -1;
        background_kind_ = BackgroundKind::background;
        background_view_ = {0.0f, 0.0f, 800.0f, 600.0f};
        background_scroll_.reset();
        background_tone_curve_.clear();
        background_brightness_ = {128.0f, 128.0f, 128.0f};
        tone_ = 0;
        tone_back_ = -1;
        tone_char_ = -1;
        weather_ = 0;
        for (std::size_t i = 0; i < overlays_.size(); ++i) {
            overlays_[i].reset();
            overlay_pixels_[i].reset();
            overlay_states_[i] = {};
        }
        clear_characters();
        sakura_.reset();

        message_ = th2::Message{};
        message_visible_ = true;
        message_ends_block_ = true;
        waiting_for_input_ = false;
        current_line_key_.clear();
        backlog_.clear();
        backlog_depth_ = 0;
        choices_.clear();
        choosing_ = false;
        choice_highlight_ = 0;
        choice_selected_ = -1;
        choice_result_register_ = -1;
        choice_ex_ = false;
        map_events_.clear();
        map_characters_.clear();
        map_selected_ = -1;

        wake_time_.reset();
        audio_wait_.reset();
        transition_.reset();
        background_fade_.reset();
        screen_flash_.reset();
        shake_.reset();
        clock_state_.reset();
        calendar_state_.reset();
        skipped_month_ = 0;
        skipped_day_ = 0;
        auto_next_time_.reset();
        auto_mode_ = false;
        skip_mode_ = false;
        demo_mode_ = false;
        replay_mode_ = false;
        demo_delay_frames_ = 0;
    }

    void initialize_scenario_flags()
    {
        runtime_.reset_flags();
        runtime_.set_flag(0, 3);
        runtime_.set_flag(1, 1);
        runtime_.set_flag(2, 0);
        runtime_.set_flag(3, -1);
        runtime_.set_flag(4, 0);
        runtime_.set_flag(
            5, th2::uses_default_voice_name(
                   config_.player_name, default_player_name_)
                ? 1 : 0);
        runtime_.set_flag(6, 0);
        runtime_.set_flag(7, 0);
    }

    void start_new_game()
    {
        reset_play_state();
        initialize_scenario_flags();
        direct_scenario_ = false;
        load_script("EV_0301MORNING.SDT");
        ui_mode_ = UiMode::game;
        advance();
    }

    void open_name_input()
    {
        name_input_open_ = true;
        name_error_.clear();
        const auto copy = [](auto& destination, const std::string& source) {
            std::snprintf(
                destination.data(), destination.size(), "%s", source.c_str());
        };
        copy(name_family_, config_.player_name.family);
        copy(name_given_, config_.player_name.given);
        copy(name_family_reading_, config_.player_name.family_reading);
        copy(name_given_reading_, config_.player_name.given_reading);
        copy(name_nickname_, config_.player_name.nickname);
    }

    void begin_title_exit(bool start_game)
    {
        if (title_exit_started_) {
            return;
        }
        title_exit_game_ = start_game;
        title_exit_started_ = std::chrono::steady_clock::now();
    }

    void begin_title_menu_transition(bool extras)
    {
        if (title_menu_transition_started_ || title_extras_ == extras) {
            return;
        }
        title_extras_transition_from_ = title_extras_;
        title_extras_ = extras;
        title_menu_transition_started_ = std::chrono::steady_clock::now();
    }

    void update_title()
    {
        if (title_menu_transition_started_) {
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                - *title_menu_transition_started_).count();
            if (elapsed * 60.0 >= 24.0) {
                title_menu_transition_started_.reset();
            }
        }
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
            open_name_input();
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

    bool volume_control(const char* label, int& volume, bool& muted)
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

    void return_to_title()
    {
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

    void draw_config()
    {
        if (!config_open_) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(570.0f, 500.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(115.0f, 50.0f), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Panel Config", &open)) {
            bool option_changed = false;
            if (ImGui::BeginTabBar("config-tabs")) {
                if (ImGui::BeginTabItem("Playback")) {
                    option_changed |= ImGui::Checkbox(
                        "Auto mode skips previously read text",
                        &config_.auto_skip_read);
                    ImGui::SliderInt(
                        "Delay between lines", &config_.auto_line_ms,
                        250, 10000, "%d ms");
                    ImGui::SliderInt(
                        "Delay at page end", &config_.auto_page_ms,
                        500, 15000, "%d ms");
                    ImGui::Separator();
                    option_changed |= ImGui::Checkbox(
                        "Auto-skip includes unread text",
                        &config_.skip_unread);
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
                    if (ImGui::Checkbox("Fullscreen", &config_.fullscreen)) {
                        SDL_SetWindowFullscreen(window_, config_.fullscreen);
                        option_changed = true;
                    }
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
            if (ImGui::BeginPopupModal(
                    "Return to Title?", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    "Unsaved progress will be lost.\nReturn to the title screen?");
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
                ImGui::EndPopup();
            }
        }
        ImGui::End();
        if (!open) {
            close_config();
        }
    }

    void draw_name_input()
    {
        if (!name_input_open_) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(430.0f, 330.0f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(400.0f, 300.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin(
            "Player Name", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        ImGui::TextUnformatted("Enter the protagonist's name.");
        ImGui::Separator();
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
        if (!name_error_.empty()) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                name_error_.c_str());
        }
        if (ImGui::Button("Start Game", ImVec2(120.0f, 0.0f))) {
            if (name_family_[0] == '\0' || name_given_[0] == '\0'
                || name_family_reading_[0] == '\0'
                || name_given_reading_[0] == '\0'
                || name_nickname_[0] == '\0') {
                name_error_ = "Every field must contain a name.";
            } else {
                config_.player_name = {
                    name_family_.data(),
                    name_given_.data(),
                    name_family_reading_.data(),
                    name_given_reading_.data(),
                    name_nickname_.data(),
                    name_nickname_.data(),
                };
                th2::save_config(config_path_, config_);
                name_input_open_ = false;
                start_new_game();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Defaults", ImVec2(130.0f, 0.0f))) {
            config_.player_name = default_player_name_;
            open_name_input();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            name_input_open_ = false;
            title_started_ = std::chrono::steady_clock::now()
                - std::chrono::milliseconds(120 * 1000 / 60);
        }
        ImGui::End();
    }

    void open_backlog()
    {
        if (choosing_) return;
        play_se(-1, 9012, false, 140);
        ui_mode_ = UiMode::backlog;
        backlog_depth_ = std::min(
            1, static_cast<int>(backlog_.size()));
    }

    void close_backlog()
    {
        backlog_depth_ = 0;
        ui_mode_ = UiMode::game;
    }

    bool backlog_older()
    {
        if (backlog_depth_ >= static_cast<int>(backlog_.size())) {
            return false;
        }
        play_se(-1, 9012, false, 140);
        ++backlog_depth_;
        ui_mode_ = UiMode::backlog;
        return true;
    }

    bool backlog_newer()
    {
        if (backlog_depth_ <= 0) {
            return false;
        }
        play_se(-1, 9012, false, 140);
        if (backlog_depth_ > 1) {
            --backlog_depth_;
        } else {
            close_backlog();
        }
        return true;
    }

    void execute_menu_item(int index)
    {
        switch (index) {
        case 0:
            if (!replay_mode_) open_save_load(UiMode::save);
            break;
        case 1:
            if (!replay_mode_) open_save_load(UiMode::load);
            break;
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
        load_error_.clear();
        refresh_save_page();
        if (newest_save_slot_ >= 0) {
            save_page_ = newest_save_slot_ / 10;
            refresh_save_page();
        }
    }

    void close_save_load()
    {
        save_confirm_slot_ = -1;
        load_error_.clear();
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

        // The original renders "<month>A<day>B" with sys0230.tga at (138, 12).
        // A and B are the month/day suffix glyphs on the sheet's second row.
        if (ui_save_digits_) {
            float x = 138.0f;
            const auto draw_date_glyph = [&](int column, int row) {
                const SDL_FRect src{
                    static_cast<float>(column * 14),
                    static_cast<float>(row * 30), 14.0f, 30.0f};
                const SDL_FRect dst{
                    x, row == 0 ? -3.0f : 12.0f, 14.0f, 30.0f};
                SDL_RenderTexture(
                    renderer_, ui_save_digits_.get(), &src, &dst);
                x += 14.0f;
            };
            const auto draw_number = [&](int value) {
                const auto digits = std::to_string(value);
                for (const char digit : digits) {
                    draw_date_glyph(digit - '0', 0);
                }
            };
            draw_number(runtime_.flag(0));
            draw_date_glyph(1, 1);
            draw_number(runtime_.flag(1));
            draw_date_glyph(2, 1);
        }

        // 4 main buttons from sys0110.tga
        // Layout: Save(0,0) Load(400,0) Hide(0,246) Settings(400,246)
        // Each: w=400, h=82, 3 states stacked vertically (0,82,164)
        const int btn_x[4] = {0, 400, 0, 400};
        const int btn_y[4] = {0, 0, 246, 246};
        const int dst_x[4] = {200, 200, 200, 200};
        const int dst_y[4] = {112, 200, 288, 376};

        for (int i = 0; i < 4; ++i) {
            const bool disabled = replay_mode_ && (i == 0 || i == 1);
            const int state = (i == menu_highlight_) ? 82 : 0;
            const SDL_FRect src{
                static_cast<float>(btn_x[i]),
                static_cast<float>(btn_y[i] + state), 400.0f, 82.0f};
            const SDL_FRect dst{
                static_cast<float>(dst_x[i]),
                static_cast<float>(dst_y[i]), 400.0f, 82.0f};
            if (ui_sys_menu_btns_) {
                SDL_SetTextureAlphaMod(
                    ui_sys_menu_btns_.get(), disabled ? 64 : 255);
                SDL_RenderTexture(renderer_, ui_sys_menu_btns_.get(),
                                  &src, &dst);
                SDL_SetTextureAlphaMod(ui_sys_menu_btns_.get(), 255);
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

    void draw_map_layer(
        int field, float x, float alpha,
        bool draw_field, bool draw_events)
    {
        if (field < 0 || field >= 5 || !map_fields_[field]) {
            return;
        }
        if (draw_field) {
            SDL_SetTextureAlphaModFloat(map_fields_[field].get(), alpha);
            const SDL_FRect field_dst{x + 80.0f, 76.0f, 640.0f, 480.0f};
            SDL_RenderTexture(
                renderer_, map_fields_[field].get(), nullptr, &field_dst);
            SDL_SetTextureAlphaModFloat(map_fields_[field].get(), 1.0f);
        }
        if (!draw_events) {
            return;
        }

        std::array<int, 10> overlaps{};
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            const auto& event = map_events_[i];
            if (event.position < 0
                || event.position >= static_cast<int>(map_positions_.size())) {
                continue;
            }
            const auto& position = map_positions_[event.position];
            const int overlap = ++overlaps[position.overlap];
            int cx = position.x;
            int cy = position.y;
            if (overlap == 2) cx -= 200;
            else if (overlap == 3) cx += 200;
            else if (overlap == 4) { cx -= 100; cy += 160; }
            if (position.field != field) {
                continue;
            }

            if (map_markers_) {
                int state = static_cast<int>(i) == map_hover_ ? 1 : 0;
                if (static_cast<int>(i) == map_selected_) state = 2;
                const SDL_FRect src{
                    static_cast<float>(state * 130),
                    static_cast<float>(event.position * 118),
                    130.0f, 118.0f};
                const SDL_FRect dst{
                    x + cx + 20.0f, static_cast<float>(cy - 118),
                    130.0f, 118.0f};
                SDL_SetTextureAlphaModFloat(map_markers_.get(), alpha);
                SDL_RenderTexture(
                    renderer_, map_markers_.get(), &src, &dst);
                SDL_SetTextureAlphaModFloat(map_markers_.get(), 1.0f);
            }
            if (i < map_characters_.size()
                && map_characters_[i].texture) {
                const auto& character = map_characters_[i];
                const auto elapsed_ticks =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - map_started_)
                        .count() * 60 / 1000;
                int cycle_ticks = 0;
                for (const auto& step : character.steps) {
                    cycle_ticks += step.ticks;
                }
                int tick = cycle_ticks > 0
                    ? static_cast<int>(elapsed_ticks % cycle_ticks) : 0;
                int frame = character.steps.front().frame;
                for (const auto& step : character.steps) {
                    if (tick < step.ticks) {
                        frame = step.frame;
                        break;
                    }
                    tick -= step.ticks;
                }
                if (frame < 0
                    || frame >= static_cast<int>(character.frames.size())) {
                    continue;
                }
                SDL_SetTextureAlphaModFloat(
                    character.texture.get(), alpha);
                for (const auto& part : character.frames[frame]) {
                    const SDL_FRect destination{
                        x + cx + part.x, cy + part.y,
                        part.source.w, part.source.h};
                    SDL_RenderTexture(
                        renderer_, character.texture.get(),
                        &part.source, &destination);
                }
                SDL_SetTextureAlphaModFloat(
                    character.texture.get(), 1.0f);
            }
        }
    }

    void draw_map(bool ui)
    {
        const float fade = map_fade_ticks_ > 0
            ? map_fade_ticks_ / 16.0f
            : std::min(1.0f,
                    std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - map_started_).count()
                    * 60.0f / 16.0f);
        if (!ui && map_frame_) {
            SDL_SetTextureAlphaModFloat(map_frame_.get(), fade);
            SDL_RenderTexture(renderer_, map_frame_.get(), nullptr, nullptr);
            SDL_SetTextureAlphaModFloat(map_frame_.get(), 1.0f);
        }

        if (map_slide_ticks_ == 0) {
            draw_map_layer(map_field_, 0.0f, fade, !ui, ui);
        } else {
            const int ticks = std::abs(map_slide_ticks_);
            const float square = static_cast<float>(ticks * ticks);
            const float direction = map_slide_ticks_ > 0 ? 1.0f : -1.0f;
            const float next_x = direction * 800.0f * square / 256.0f;
            const float previous_x =
                direction * 800.0f * (square - 256.0f) / 256.0f;
            draw_map_layer(map_field_, next_x, fade, !ui, ui);
            draw_map_layer(
                map_previous_field_, previous_x, fade, !ui, ui);
        }

        if (ui && map_arrows_) {
            const int left_state = map_hover_ == -2 ? 1 : 0;
            const int right_state = map_hover_ == -3 ? 1 : 0;
            const SDL_FRect left_src{
                static_cast<float>(left_state * 112), 0.0f, 56.0f, 122.0f};
            const SDL_FRect right_src{
                static_cast<float>(right_state * 112 + 56), 0.0f,
                56.0f, 122.0f};
            const SDL_FRect left_dst{24.0f, 239.0f, 56.0f, 122.0f};
            const SDL_FRect right_dst{720.0f, 239.0f, 56.0f, 122.0f};
            SDL_SetTextureAlphaModFloat(map_arrows_.get(), fade);
            SDL_RenderTexture(
                renderer_, map_arrows_.get(), &left_src, &left_dst);
            SDL_RenderTexture(
                renderer_, map_arrows_.get(), &right_src, &right_dst);
            SDL_SetTextureAlphaModFloat(map_arrows_.get(), 1.0f);
        }
    }

    bool title_extras_available() const
    {
        return runtime_.game_flag(80) != 0 || runtime_.game_flag(87) != 0;
    }

    bool title_item_disabled(int item) const
    {
        if (title_extras_) {
            return item == 3;
        }
        return item == 3 && !title_extras_available();
    }

    static constexpr std::array<int, 649> cg_gallery_layout{
        1010, 1011, 0, 1020, 0, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 0, 1040, 1041, 1042,
        0, 1050, 0, 1060, 0, 1070, 1071, 0, 1080, 0, 1090, 0, 1100, 1101, 0, 1110,
        0, 1120, 0, 1130, 1131, 0, 1140, 0, 1150, 1151, 1152, 1153, 0, 1160, 0, 1170,
        0, 1990, 1991, 0, 2010, 2011, 2012, 2013, 2014, 2015, 0, 2020, 2021, 2022, 2023, 2024,
        0, 2030, 2031, 2032, 0, 2040, 2041, 2042, 0, 2050, 2051, 2052, 2053, 2054, 2055, 2056,
        2057, 2058, 0, 2060, 0, 2070, 0, 2080, 2081, 2082, 0, 2090, 0, 2100, 2101, 0,
        2110, 2111, 0, 2120, 0, 2140, 2141, 0, 2150, 0, 2160, 0, 2170, 0, 2180, 0,
        2190, 0, 3010, 3011, 3012, 0, 3020, 3021, 0, 3030, 3031, 3032, 0, 3040, 0, 3050,
        3051, 0, 3060, 0, 3070, 3071, 3072, 0, 3080, 3081, 3082, 3083, 3084, 3085, 0, 3090,
        3086, 0, 3100, 3101, 3102, 3103, 3104, 3105, 0, 3110, 0, 3120, 0, 3130, 3131, 3132,
        3133, 3134, 3135, 0, 3140, 0, 3150, 3151, 0, 4010, 0, 4020, 4021, 4022, 0, 4030,
        4031, 4032, 4033, 0, 4040, 0, 4050, 4051, 4052, 0, 4060, 0, 4070, 0, 4080, 4081,
        0, 4090, 4091, 4092, 4093, 4094, 0, 4100, 4101, 4102, 4103, 0, 4110, 4111, 4112, 0,
        4120, 4121, 0, 4130, 0, 4140, 0, 4150, 0, 4160, 4161, 4162, 0, 5010, 0, 5020,
        0, 5030, 0, 5040, 0, 5050, 0, 5060, 5061, 0, 5070, 0, 5080, 0, 5090, 5091,
        0, 5100, 0, 5110, 0, 5120, 0, 5130, 0, 5140, 0, 5150, 0, 5160, 0, 7010,
        7011, 7012, 0, 7020, 0, 7030, 0, 7040, 0, 7050, 0, 7060, 0, 7070, 7071, 0,
        7080, 0, 7090, 7091, 7092, 7093, 0, 7100, 7101, 7102, 0, 7110, 7111, 7112, 0, 7120,
        0, 7130, 0, 7140, 0, 7150, 0, 7160, 0, 8010, 0, 8020, 0, 8030, 0, 8040,
        8041, 0, 8050, 0, 8060, 0, 8070, 8071, 0, 8080, 0, 8090, 0, 8100, 0, 8110,
        0, 8120, 0, 8130, 0, 8140, 0, 8150, 0, 9010, 9011, 0, 9020, 9021, 9022, 0,
        9030, 9031, 9032, 0, 9050, 9051, 0, 9060, 0, 9070, 0, 9080, 9081, 0, 9090, 9091,
        9092, 0, 9100, 0, 9110, 0, 9120, 9121, 9122, 9123, 9124, 0, 10010, 10011, 0, 15000,
        0, 28010, 0, 28020, 28021, 28022, 0, 28030, 28031, 28032, 0, 28040, 0, 28050, 0, 28060,
        28061, 28062, 28063, 28064, 28065, 28066, 28067, 0, 28070, 28071, 0, 28080, 28081, 0, 28090, 0,
        28100, 0, 28110, 28111, 0, 28120, 0, 28130, 28131, 0, 101000, 101001, 0, 101010, 101011, 101012,
        101013, 101014, 101015, 0, 101020, 101021, 101022, 101023, 101024, 0, 101030, 101031, 101032, 101033, 101034, 101035,
        0, 101040, 101041, 101042, 101043, 101044, 0, 102000, 102001, 0, 102010, 0, 102020, 102021, 102022, 0,
        102030, 102031, 0, 102040, 102041, 0, 103000, 103001, 103002, 103003, 103004, 103005, 103006, 103007, 103008, 0,
        103010, 103011, 103012, 103013, 103014, 0, 103020, 103021, 103022, 103023, 103024, 103025, 103026, 103027, 103028, 103029,
        0, 103030, 103031, 103032, 103033, 0, 103040, 103041, 103042, 103043, 103044, 0, 103120, 103121, 103122, 0,
        103103, 103104, 103105, 103106, 103107, 103108, 0, 103130, 103131, 103132, 103133, 0, 103140, 103141, 103142, 103143,
        103144, 0, 103203, 103204, 103205, 103206, 103207, 103208, 0, 103220, 103221, 103222, 0, 103230, 103231, 103232,
        103233, 0, 103240, 103241, 103242, 103243, 103244, 0, 104000, 104001, 104002, 104003, 0, 104010, 104011, 104012,
        104013, 0, 104020, 104021, 0, 104030, 104031, 104032, 104033, 104034, 104035, 0, 105000, 0, 105010, 105011,
        105012, 105013, 0, 105020, 105021, 105022, 0, 105030, 105031, 105032, 105033, 105034, 105035, 0, 105040, 105041,
        0, 107000, 107001, 107002, 107003, 0, 107010, 0, 107020, 107021, 107022, 0, 107030, 107031, 0, 107040,
        107041, 0, 108000, 0, 108010, 108011, 108012, 108013, 0, 108020, 108021, 108022, 0, 108030, 0, 109000,
        109001, 109002, 109003, 109004, 109005, 109006, 109007, 0, 109010, 109011, 109012, 109013, 0, 109020, 109021, 109022,
        109023, 109024, 109025, 109026, 109027, 0, 109030, 109031, 109032, 0, 128000, 128001, 0, 128010, 128011, 0,
        128020, 128021, 128022, 128023, 0, 0, 0, 0, 0,
    };

    void draw_omake_cancel(int highlight)
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

    void open_cg_gallery()
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
                    ? config_.unlocked_h_cgs
                    : config_.unlocked_visual_cgs;
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

    void open_music_room()
    {
        begin_transition(1, 30, 128, false);
        bgm_.fade_to(0.0f, std::chrono::milliseconds(500), true);
        bgm_track_ = -1;
        omake_highlight_ = 40;
        omake_music_playing_slot_ = -1;
        ui_mode_ = UiMode::music_room;
    }

    void open_replay_gallery()
    {
        begin_transition(1, 30, 128, false);
        for (int slot = 0; slot < 9; ++slot) {
            omake_replay_thumbnails_[slot].reset();
            if (!config_.unlocked_replays.contains(replay_flags[slot])) {
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

    void close_omake_screen()
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

    void draw_cg_gallery_page()
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

    void draw_cg_full(
        SDL_Texture* texture, float source_y, const SDL_FRect& destination,
        float alpha = 1.0f)
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

    float omake_cg_phase_progress(int frames) const
    {
        return std::clamp(
            static_cast<float>(
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - omake_cg_phase_started_).count()
                * 60.0 / frames),
            0.0f, 1.0f);
    }

    void draw_cg_gallery()
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

    static constexpr std::array<int, 40> music_room_tracks{
        0, 10, 29, 11, 12, 13, 14, 30, 27, 1,
        2, 4, 3, 5, 6, 8, 7, 9, 18, 37,
        38, 41, 42, 39, 40, 15, 16, 17, 19, 20,
        22, 32, 21, 23, 26, 31, 25, 24, 28, 50,
    };
    static constexpr std::array<std::array<int, 2>, 40>
        music_room_artists{{
            {3, 5}, {3, 3}, {4, 4}, {2, 2}, {1, 1},
            {2, 2}, {1, 1}, {0, 0}, {2, 2}, {2, 2},
            {0, 0}, {3, 3}, {0, 0}, {4, 4}, {4, 4},
            {2, 2}, {1, 1}, {1, 1}, {2, 2}, {6, 4},
            {6, 4}, {1, 1}, {0, 0}, {5, 5}, {0, 0},
            {3, 3}, {3, 3}, {3, 3}, {2, 2}, {2, 2},
            {1, 1}, {4, 4}, {2, 2}, {1, 1}, {2, 2},
            {3, 3}, {1, 1}, {1, 1}, {3, 3}, {2, 5},
        }};

    void draw_music_room()
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

    static constexpr std::array<int, 9> replay_flags{
        1, 2, 3, 4, 5, 7, 8, 9, 11,
    };
    static constexpr std::array<int, 9> replay_scripts{
        10, 20, 30, 40, 50, 70, 80, 90, 110,
    };
    static constexpr std::array<int, 9> replay_thumbnails{
        1000, 2010, 3000, 4000, 5000, 7010, 8000, 9000, 28000,
    };

    void draw_replay_gallery()
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
                config_.unlocked_replays.contains(replay_flags[slot]);
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

    void activate_title_item()
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

    void handle_title_input(const SDL_Event& event)
    {
        const int frame = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - title_started_).count()
            * 60 / 1000);
        if (frame < 120 || transition_ || title_exit_started_
            || title_menu_transition_started_) {
            return;
        }
        const int previous_highlight = title_highlight_;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_UP) {
                do {
                    title_highlight_ = (title_highlight_ + 4) % 5;
                } while (title_item_disabled(title_highlight_));
            } else if (event.key.key == SDLK_DOWN) {
                do {
                    title_highlight_ = (title_highlight_ + 1) % 5;
                } while (title_item_disabled(title_highlight_));
            } else if (event.key.key == SDLK_RETURN
                       || event.key.key == SDLK_SPACE) {
                activate_title_item();
            } else if (event.key.key == SDLK_ESCAPE) {
                if (title_extras_) {
                    begin_title_menu_transition(false);
                    title_highlight_ = 3;
                } else {
                    title_highlight_ = 4;
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            const float x = event.motion.x;
            const float y = event.motion.y;
            if (x >= 306.0f && x < 494.0f) {
                for (int i = 0; i < 5; ++i) {
                    if (title_item_disabled(i)) continue;
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
                    if (title_item_disabled(i)) continue;
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

    void activate_cg_gallery_item()
    {
        const int page_count = std::max(
            1, (static_cast<int>(omake_cg_entries_.size()) + 11) / 12);
        if (omake_highlight_ >= 0 && omake_highlight_ < 12) {
            const int index = omake_page_ * 12 + omake_highlight_;
            if (index >= static_cast<int>(omake_cg_entries_.size())
                || omake_cg_entries_[index].variants.empty()) {
                return;
            }
            const auto& entry = omake_cg_entries_[index];
            omake_cg_variant_ = 0;
            omake_cg_tall_scrolled_ = false;
            const int cg = entry.variants.front();
            omake_cg_full_ = load_texture(
                renderer_, graphics_,
                std::format("{}{:06d}.tga", entry.hcg ? 'h' : 'v', cg));
            omake_cg_view_ = index;
            omake_cg_phase_ = OmakeCgPhase::opening;
            omake_cg_phase_started_ = std::chrono::steady_clock::now();
            play_se(-1, 9104, false, 255);
        } else if (omake_highlight_ == 12) {
            begin_transition(1, 8, 128, false);
            omake_page_ = (omake_page_ + page_count - 1) % page_count;
            play_se(-1, 9104, false, 255);
        } else if (omake_highlight_ == 13) {
            begin_transition(1, 8, 128, false);
            omake_page_ = (omake_page_ + 1) % page_count;
            play_se(-1, 9104, false, 255);
        } else if (omake_highlight_ == 14) {
            play_se(-1, 9107, false, 255);
            close_omake_screen();
        }
    }

    void handle_cg_gallery_input(const SDL_Event& event)
    {
        if (transition_) {
            return;
        }
        if (omake_cg_view_) {
            if (omake_cg_phase_ != OmakeCgPhase::viewing) {
                return;
            }
            const bool cancel = event.type == SDL_EVENT_KEY_DOWN
                    && event.key.key == SDLK_ESCAPE
                || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                    && event.button.button == SDL_BUTTON_RIGHT;
            const bool advance = event.type == SDL_EVENT_KEY_DOWN
                    && (event.key.key == SDLK_RETURN
                        || event.key.key == SDLK_SPACE
                        || event.key.key == SDLK_RIGHT)
                || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                    && event.button.button == SDL_BUTTON_LEFT;
            if (cancel) {
                omake_cg_phase_ = OmakeCgPhase::closing;
                omake_cg_phase_started_ = std::chrono::steady_clock::now();
                play_se(-1, 9107, false, 255);
            } else if (advance) {
                const auto& entry =
                    omake_cg_entries_.at(*omake_cg_view_);
                float height = 0.0f;
                SDL_GetTextureSize(omake_cg_full_.get(), nullptr, &height);
                if (height > 600.0f) {
                    omake_cg_phase_ = omake_cg_tall_scrolled_
                        ? OmakeCgPhase::closing
                        : OmakeCgPhase::scrolling;
                    omake_cg_phase_started_ =
                        std::chrono::steady_clock::now();
                    play_se(
                        -1, omake_cg_tall_scrolled_ ? 9107 : 9104,
                        false, 255);
                    return;
                }
                ++omake_cg_variant_;
                if (omake_cg_variant_
                    >= static_cast<int>(entry.variants.size())) {
                    omake_cg_phase_ = OmakeCgPhase::closing;
                    omake_cg_phase_started_ =
                        std::chrono::steady_clock::now();
                    play_se(-1, 9107, false, 255);
                } else {
                    omake_cg_tall_scrolled_ = false;
                    omake_cg_previous_full_ = std::move(omake_cg_full_);
                    omake_cg_full_ = load_texture(
                        renderer_, graphics_,
                        std::format(
                            "{}{:06d}.tga", entry.hcg ? 'h' : 'v',
                            entry.variants[omake_cg_variant_]));
                    omake_cg_phase_ = OmakeCgPhase::changing;
                    omake_cg_phase_started_ =
                        std::chrono::steady_clock::now();
                    play_se(-1, 9104, false, 255);
                }
            }
            return;
        }
        const int previous = omake_highlight_;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                omake_highlight_ = 14;
                activate_cg_gallery_item();
                return;
            }
            if (event.key.key == SDLK_LEFT) {
                if (omake_highlight_ < 12) {
                    omake_highlight_ = std::max(0, omake_highlight_ - 1);
                } else if (omake_highlight_ == 13) {
                    omake_highlight_ = 12;
                }
            } else if (event.key.key == SDLK_RIGHT) {
                if (omake_highlight_ < 12) {
                    omake_highlight_ = std::min(11, omake_highlight_ + 1);
                } else if (omake_highlight_ == 12) {
                    omake_highlight_ = 13;
                }
            } else if (event.key.key == SDLK_UP) {
                if (omake_highlight_ == 14) {
                    omake_highlight_ = 11;
                } else if (omake_highlight_ < 4) {
                    omake_highlight_ = omake_highlight_ < 3 ? 12 : 13;
                } else if (omake_highlight_ < 12) {
                    omake_highlight_ -= 4;
                }
            } else if (event.key.key == SDLK_DOWN) {
                if (omake_highlight_ == 12) {
                    omake_highlight_ = 2;
                } else if (omake_highlight_ == 13) {
                    omake_highlight_ = 3;
                } else if (omake_highlight_ >= 8
                           && omake_highlight_ < 12) {
                    omake_highlight_ = 14;
                } else if (omake_highlight_ < 8) {
                    omake_highlight_ += 4;
                }
            } else if (event.key.key == SDLK_RETURN
                       || event.key.key == SDLK_SPACE) {
                activate_cg_gallery_item();
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION
                   || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const float x = event.type == SDL_EVENT_MOUSE_MOTION
                ? event.motion.x : event.button.x;
            const float y = event.type == SDL_EVENT_MOUSE_MOTION
                ? event.motion.y : event.button.y;
            for (int slot = 0; slot < 12; ++slot) {
                const SDL_FRect rectangle{
                    55.0f + 177.0f * (slot % 4),
                    114.0f + 128.0f * (slot / 4), 160.0f, 120.0f};
                if (x >= rectangle.x && x < rectangle.x + rectangle.w
                    && y >= rectangle.y && y < rectangle.y + rectangle.h) {
                    omake_highlight_ = slot;
                }
            }
            if (x >= 190.0f && x < 320.0f && y >= 72.0f && y < 104.0f) {
                omake_highlight_ = 12;
            } else if (x >= 482.0f && x < 612.0f
                       && y >= 72.0f && y < 104.0f) {
                omake_highlight_ = 13;
            } else if (x >= 306.0f && x < 494.0f
                       && y >= 496.0f && y < 528.0f) {
                omake_highlight_ = 14;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && event.button.button == SDL_BUTTON_LEFT) {
                activate_cg_gallery_item();
            }
        }
        if (previous != omake_highlight_) {
            play_se(-1, 9108, false, 255);
        }
    }

    void activate_music_room_item()
    {
        if (omake_highlight_ == 40) {
            play_se(-1, 9107, false, 255);
            close_omake_screen();
            return;
        }
        if (omake_highlight_ < 0 || omake_highlight_ >= 40
            || runtime_.game_flag(128 + omake_highlight_) == 0) {
            return;
        }
        play_se(-1, 9104, false, 255);
        if (omake_music_playing_slot_ == omake_highlight_) {
            bgm_.fade_to(0.0f, std::chrono::seconds(1), true);
            bgm_track_ = -1;
            omake_music_playing_slot_ = -1;
        } else {
            omake_music_playing_slot_ = omake_highlight_;
            play_bgm(
                music_room_tracks[omake_music_playing_slot_], true, 255);
        }
    }

    void handle_music_room_input(const SDL_Event& event)
    {
        if (transition_) {
            return;
        }
        const int previous = omake_highlight_;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                omake_highlight_ = 40;
                activate_music_room_item();
                return;
            }
            if (event.key.key == SDLK_UP && omake_highlight_ < 40) {
                omake_highlight_ = std::max(0, omake_highlight_ - 1);
            } else if (event.key.key == SDLK_DOWN
                       && omake_highlight_ < 40) {
                omake_highlight_ = std::min(39, omake_highlight_ + 1);
            } else if (event.key.key == SDLK_LEFT) {
                if (omake_highlight_ == 40) {
                    omake_highlight_ = 39;
                } else {
                    omake_highlight_ = std::max(0, omake_highlight_ - 10);
                }
            } else if (event.key.key == SDLK_RIGHT) {
                omake_highlight_ =
                    omake_highlight_ >= 30 ? 40 : omake_highlight_ + 10;
            } else if (event.key.key == SDLK_RETURN
                       || event.key.key == SDLK_SPACE) {
                activate_music_room_item();
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION
                   || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const float x = event.type == SDL_EVENT_MOUSE_MOTION
                ? event.motion.x : event.button.x;
            const float y = event.type == SDL_EVENT_MOUSE_MOTION
                ? event.motion.y : event.button.y;
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 10; ++row) {
                    const SDL_FRect rectangle{
                        static_cast<float>(20 + column * 192),
                        static_cast<float>(160 + row * 32), 184.0f, 28.0f};
                    if (x >= rectangle.x && x < rectangle.x + rectangle.w
                        && y >= rectangle.y && y < rectangle.y + rectangle.h) {
                        omake_highlight_ = column * 10 + row;
                    }
                }
            }
            if (x >= 306.0f && x < 494.0f
                && y >= 496.0f && y < 528.0f) {
                omake_highlight_ = 40;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && event.button.button == SDL_BUTTON_LEFT) {
                activate_music_room_item();
            }
        }
        if (previous != omake_highlight_) {
            play_se(-1, 9108, false, 255);
        }
    }

    void start_replay(int slot)
    {
        reset_play_state();
        initialize_scenario_flags();
        direct_scenario_ = false;
        replay_mode_ = true;
        load_script(std::format(
            "8000{:05d}.SDT", replay_scripts.at(slot)));
        ui_mode_ = UiMode::game;
        title_extras_ = false;
        advance();
    }

    void activate_replay_gallery_item()
    {
        if (omake_highlight_ == 14) {
            play_se(-1, 9107, false, 255);
            close_omake_screen();
            return;
        }
        if (omake_highlight_ < 0 || omake_highlight_ >= 9
            || !config_.unlocked_replays.contains(
                replay_flags[omake_highlight_])) {
            return;
        }
        play_se(-1, 9104, false, 255);
        start_replay(omake_highlight_);
    }

    void handle_replay_gallery_input(const SDL_Event& event)
    {
        if (transition_) {
            return;
        }
        const int previous = omake_highlight_;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                omake_highlight_ = 14;
                activate_replay_gallery_item();
                return;
            }
            if (event.key.key == SDLK_LEFT && omake_highlight_ < 9) {
                omake_highlight_ = std::max(0, omake_highlight_ - 1);
            } else if (event.key.key == SDLK_RIGHT
                       && omake_highlight_ < 9) {
                omake_highlight_ = std::min(8, omake_highlight_ + 1);
            } else if (event.key.key == SDLK_UP) {
                if (omake_highlight_ == 14) {
                    omake_highlight_ = 7;
                } else if (omake_highlight_ >= 4) {
                    omake_highlight_ -= 4;
                }
            } else if (event.key.key == SDLK_DOWN) {
                if (omake_highlight_ >= 4 && omake_highlight_ < 9) {
                    omake_highlight_ = 14;
                } else if (omake_highlight_ < 4) {
                    omake_highlight_ += 4;
                }
            } else if (event.key.key == SDLK_RETURN
                       || event.key.key == SDLK_SPACE) {
                activate_replay_gallery_item();
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION
                   || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const float x = event.type == SDL_EVENT_MOUSE_MOTION
                ? event.motion.x : event.button.x;
            const float y = event.type == SDL_EVENT_MOUSE_MOTION
                ? event.motion.y : event.button.y;
            for (int slot = 0; slot < 9; ++slot) {
                const SDL_FRect rectangle{
                    55.0f + 177.0f * (slot % 4),
                    114.0f + 128.0f * (slot / 4), 160.0f, 120.0f};
                if (x >= rectangle.x && x < rectangle.x + rectangle.w
                    && y >= rectangle.y && y < rectangle.y + rectangle.h) {
                    omake_highlight_ = slot;
                }
            }
            if (x >= 306.0f && x < 494.0f
                && y >= 496.0f && y < 528.0f) {
                omake_highlight_ = 14;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && event.button.button == SDL_BUTTON_LEFT) {
                activate_replay_gallery_item();
            }
        }
        if (previous != omake_highlight_) {
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

        if (!load_error_.empty()) {
            font_.draw(renderer_, 21.0f, 571.0f, load_error_, 0, 0, 0);
            font_.draw(renderer_, 20.0f, 570.0f, load_error_, 255, 80, 80);
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
        load_error_.clear();
        if (item >= 0 && item < 10) {
            const int slot = save_page_ * 10 + item;
            if (ui_mode_ == UiMode::load && !visible_saves_[item].exists) {
                return;
            }
            play_se(-1, 9104, false, 255);
            if (ui_mode_ == UiMode::save && !visible_saves_[item].exists) {
                save(slot);
                close_save_load();
            } else {
                save_confirm_slot_ = slot;
                save_hover_ = 14;
            }
        } else if (item == 10 || item == 11) {
            play_se(-1, 9104, false, 255);
            save_page_ = (save_page_ + (item == 10 ? 9 : 1)) % 10;
            refresh_save_page();
        } else if (item == 12) {
            play_se(-1, 9104, false, 255);
            close_save_load();
        } else if (item == 13 && save_confirm_slot_ >= 0) {
            play_se(-1, 9104, false, 255);
            const int slot = save_confirm_slot_;
            if (ui_mode_ == UiMode::save) {
                save(slot);
                close_save_load();
            } else if (load(slot)) {
                save_confirm_slot_ = -1;
                ui_mode_ = UiMode::game;
            } else {
                load_error_ = "Incompatible save version.";
            }
        } else if (item == 14) {
            play_se(-1, 9104, false, 255);
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
                play_se(-1, 9107, false, 255);
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
                play_se(-1, 9107, false, 255);
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
        const auto enabled = [&](int item) {
            return !replay_mode_ || (item != 0 && item != 1);
        };
        const auto move_highlight = [&](int direction) {
            do {
                menu_highlight_ =
                    (menu_highlight_ + direction + 5) % 5;
            } while (!enabled(menu_highlight_));
        };

        const int previous_highlight = menu_highlight_;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_ESCAPE) {
                play_se(-1, 9107, false, 255);
                close_system_menu();
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE) {
                const int item = menu_highlight_;
                if (enabled(item)) {
                    play_se(-1, 9014, false, 255);
                    close_system_menu();
                    execute_menu_item(item);
                }
            } else if (event.key.key == SDLK_UP) {
                move_highlight(-1);
            } else if (event.key.key == SDLK_DOWN) {
                move_highlight(1);
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
                play_se(-1, 9107, false, 255);
                close_system_menu();
            } else if (event.button.button == SDL_BUTTON_LEFT) {
                const float mx = event.button.x; const float my = event.button.y;
                for (int i = 0; i < 5; ++i) {
                    if (mx >= dst_x[i] && mx < dst_x[i] + dst_w[i]
                        && my >= dst_y[i] && my < dst_y[i] + dst_h[i]
                        && enabled(i)) {
                        play_se(-1, 9014, false, 255);
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
                    && my >= dst_y[i] && my < dst_y[i] + dst_h[i]
                    && enabled(i)) {
                    menu_highlight_ = i;
                    break;
                }
            }
        }
        if (menu_highlight_ != previous_highlight) {
            play_se(-1, 9108, false, 255);
        }
    }

    void change_map_field(int direction)
    {
        if (map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
            return;
        }
        map_previous_field_ = map_field_;
        do {
            map_field_ = (map_field_ + direction + 5) % 5;
        } while (!map_fields_[map_field_]);
        map_slide_ticks_ = direction > 0 ? 16 : -16;
        map_hover_ = -1;
        play_se(-1, 9015, false, 255);
    }

    void update_map_hover(float x, float y)
    {
        map_hover_ = -1;
        if (x >= 24.0f && x < 80.0f && y >= 239.0f && y < 361.0f) {
            map_hover_ = -2;
            return;
        }
        if (x >= 720.0f && x < 776.0f && y >= 239.0f && y < 361.0f) {
            map_hover_ = -3;
            return;
        }
        std::array<int, 10> overlaps{};
        for (std::size_t i = 0; i < map_events_.size(); ++i) {
            const auto& event = map_events_[i];
            if (event.position < 0
                || event.position >= static_cast<int>(map_positions_.size())) {
                continue;
            }
            const auto& position = map_positions_[event.position];
            const int overlap = ++overlaps[position.overlap];
            int cx = position.x;
            int cy = position.y;
            if (overlap == 2) cx -= 200;
            else if (overlap == 3) cx += 200;
            else if (overlap == 4) { cx -= 100; cy += 160; }
            if (position.field == map_field_
                && x >= cx + 20 && x < cx + 150
                && y >= cy - 118 && y < cy) {
                map_hover_ = static_cast<int>(i);
                return;
            }
        }
    }

    void handle_map_input(const SDL_Event& event)
    {
        if (map_slide_ticks_ != 0 || map_fade_ticks_ != 0) {
            return;
        }
        const int previous = map_hover_;
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            update_map_hover(event.motion.x, event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                   && event.button.button == SDL_BUTTON_LEFT) {
            update_map_hover(event.button.x, event.button.y);
            if (map_hover_ == -2) {
                change_map_field(1);
            } else if (map_hover_ == -3) {
                change_map_field(-1);
            } else if (map_hover_ >= 0) {
                finish_map_selection(map_hover_);
            }
        } else if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_PAGEUP
                || event.key.key == SDLK_RIGHT) {
                change_map_field(1);
            } else if (event.key.key == SDLK_PAGEDOWN
                       || event.key.key == SDLK_LEFT) {
                change_map_field(-1);
            } else if ((event.key.key == SDLK_RETURN
                        || event.key.key == SDLK_SPACE)
                       && map_hover_ >= 0) {
                finish_map_selection(map_hover_);
            }
        }
        if (map_hover_ != previous && map_hover_ != -1) {
            play_se(-1, 9108, false, 255);
        }
    }

    void update_map()
    {
        if (ui_mode_ != UiMode::map) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        constexpr auto tick = std::chrono::microseconds(1'000'000 / 60);
        while (now - map_tick_ >= tick) {
            map_tick_ += tick;
            if (map_slide_ticks_ > 0) {
                --map_slide_ticks_;
            } else if (map_slide_ticks_ < 0) {
                ++map_slide_ticks_;
            }
            if (map_fade_ticks_ > 0) {
                --map_fade_ticks_;
                if (map_fade_ticks_ == 0) {
                    complete_map_selection();
                    return;
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

        const float x = message_text_x();
        float y = message_text_y();
        for (const auto& line : display_lines(selected)) {
            font_.draw(renderer_, x + 2.0f, y + 2.0f, line, 0, 0, 0);
            font_.draw(renderer_, x, y, line, 255, 144, 32);
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
            const bool disabled = replay_mode_ && (i == 2 || i == 3);
            const bool active =
                (i == 4 && auto_mode_) || (i == 5 && skip_mode_);
            const float state_x = disabled ? 0.0f : active ? 66.0f
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
                if (replay_mode_ && (i == 2 || i == 3)) {
                    return;
                }
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
                backlog_older();
                break;
            case 1:
                if (!backlog_newer()) {
                    advance();
                }
                break;
            case 2:
                if (replay_mode_) break;
                play_se(-1, 9104, false, 255);
                save_snapshot_ = capture_frame_pixels();
                open_save_load(UiMode::save);
                break;
            case 3:
                if (replay_mode_) break;
                play_se(-1, 9104, false, 255);
                save_snapshot_ = capture_frame_pixels();
                open_save_load(UiMode::load);
                break;
            case 4:
                play_se(-1, 9104, false, 255);
                auto_mode_ = !auto_mode_;
                if (auto_mode_) skip_mode_ = false;
                break;
            case 5:
                play_se(-1, 9104, false, 255);
                skip_mode_ = !skip_mode_;
                if (skip_mode_) auto_mode_ = false;
                break;
            case 6:
                play_se(-1, 9104, false, 255);
                open_config();
                break;
            case 7:
                if (replay_mode_) break;
                play_se(-1, 9104, false, 255);
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
                backlog_older();
            } else if (event.key.key == SDLK_DOWN) {
                backlog_newer();
            } else if (event.key.key == SDLK_PAGEUP) {
                backlog_older();
            } else if (event.key.key == SDLK_PAGEDOWN) {
                backlog_newer();
            } else if (event.key.key == SDLK_HOME) {
                if (backlog_depth_ != static_cast<int>(backlog_.size())) {
                    play_se(-1, 9012, false, 140);
                    backlog_depth_ = static_cast<int>(backlog_.size());
                }
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
                backlog_older();
            } else if (event.wheel.y < 0) {
                backlog_newer();
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
        const float width = font_.text_width(last_line);

        const float x = message_text_x() + width + 4.0f;
        const float y = message_text_y()
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

    void begin_overlay()
    {
        auto* overlay = upscaler_->overlay_target();
        SDL_SetRenderTarget(renderer_, overlay);
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(overlay, &width, &height);
        SDL_SetRenderScale(renderer_, width / 800.0f, height / 600.0f);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }

    void draw_overlay(std::size_t slot)
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

    void present_frame()
    {
        upscaler_->present();

        // ImGui is rendered directly to the window backbuffer using the OS
        // display scale, so the debug/config UI stays crisp but does not
        // balloon to the full screen magnification used for the 800x600 art.
        const float display_scale = std::max(
            1.0f, SDL_GetWindowDisplayScale(window_));
        SDL_SetRenderTarget(renderer_, nullptr);
        SDL_SetRenderScale(renderer_, display_scale, display_scale);
        imgui_->render();

        SDL_RenderPresent(renderer_);
    }

    void ensure_shake_target()
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

    void draw()
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
        if (ui_mode_ != UiMode::backlog
            && message_visible_ && !message_.empty()) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 16, 150);
            SDL_RenderFillRect(renderer_, nullptr);
            const auto lines = display_lines(message_.visible());
            const float x = message_text_x();
            float y = message_text_y();
            for (const auto& line : lines) {
                font_.draw(renderer_, x + 2.0f, y + 2.0f, line, 0, 0, 0);
                font_.draw(renderer_, x, y, line);
                y += 31.0f;
                if (y > 535.0f) {
                    break;
                }
            }
        }
        if (ui_mode_ != UiMode::backlog
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
                    y += 31.0f;
                }
            }
        }
        SDL_SetRenderViewport(renderer_, nullptr);
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
};

}  // namespace

int main(int argc, char** argv)
{
    try {
        std::filesystem::path data = "game-data";
        std::optional<std::filesystem::path> scenario;
        bool data_set = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--scenario") {
                if (++index >= argc) {
                    throw std::runtime_error("--scenario requires an SDT path");
                }
                scenario = argv[index];
            } else if (!data_set) {
                data = argv[index];
                data_set = true;
            } else {
                throw std::runtime_error(
                    "usage: toheart2 [GAME_DATA_DIRECTORY] [--scenario FILE.SDT]");
            }
        }
        return Game(data, scenario).run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
