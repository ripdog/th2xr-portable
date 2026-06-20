#include "game.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_video.h>

extern "C" {
    #include <libavutil/log.h>
}

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace th2app {

namespace {

std::filesystem::path ensure_parent_directory(std::filesystem::path path)
{
    std::filesystem::create_directories(path.parent_path());
    return path;
}

}  // namespace

struct SdlSubsystem {
    SdlSubsystem()
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    ~SdlSubsystem() { SDL_Quit(); }

    SdlSubsystem(const SdlSubsystem&) = delete;
    SdlSubsystem& operator=(const SdlSubsystem&) = delete;
};

Game::Game(
    const std::filesystem::path& data,
    const std::optional<std::filesystem::path>& scenario,
    const std::optional<std::filesystem::path>& soak_directory,
    std::size_t soak_runs)
    : scripts_(data / "SDT.PAK"), graphics_(data / "GRP.PAK"),
      backgrounds_(data / "bak.pak"), fonts_(data / "FNT.PAK"),
      bgm_archive_(data / "bgm.PAK"), se_archive_(data / "SE.PAK"),
      voice_archive_(data / "voice.pak"), movie_archive_(data / "mov.pak"),
      runtime_(scripts_),
      config_path_(ensure_parent_directory(soak_directory
          ? *soak_directory / "config.ini"
          : profile_directory() / "toheart2-config.ini")),
      state_path_(ensure_parent_directory(soak_directory
          ? *soak_directory / "state.sqlite3"
          : profile_directory() / "toheart2-state.sqlite3")),
      config_(th2::load_config(config_path_)),
      persistent_state_(state_path_),
      suppress_audio_output_(soak_directory.has_value()),
      font_(fonts_)
{
    SDL_Log("Config path: %s", config_path_.string().c_str());
    SDL_Log("State path: %s", state_path_.string().c_str());
    persistent_game_flags_ = persistent_state_.load_game_flags();
    unlocked_visual_cgs_ = persistent_state_.load_unlocks(
        th2::PersistentState::UnlockKind::visual_cg);
    unlocked_h_cgs_ = persistent_state_.load_unlocks(
        th2::PersistentState::UnlockKind::h_cg);
    unlocked_replays_ = persistent_state_.load_unlocks(
        th2::PersistentState::UnlockKind::replay);
    const auto non_zero_flags = std::ranges::count_if(
        persistent_game_flags_, [](std::int32_t v) { return v != 0; });
    SDL_Log(
        "Loaded %zu non-zero game flags (flag98=%d)",
        static_cast<std::size_t>(non_zero_flags), persistent_game_flags_[98]);
    default_player_name_ =
        th2::load_default_player_name(data / "TOHEART2.EXE");
    player_name_ = default_player_name_;
    for (std::size_t i = 0; i < persistent_game_flags_.size(); ++i) {
        runtime_.set_game_flag(i, persistent_game_flags_[i]);
    }
    window_ = SDL_CreateWindow(
        "ToHeart2 XRATED", config_.window_width, config_.window_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    window_holder_.reset(window_);
    if (window_ && config_.window_x >= 0 && config_.window_y >= 0) {
        SDL_SetWindowPosition(window_, config_.window_x, config_.window_y);
    }
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
    if (auto icon = th2::load_executable_icon(data / "TOHEART2.EXE")) {
        SDL_SetWindowIcon(window_, icon.get());
    }
    SDL_Log("SDL window and renderer created");
#ifndef __ANDROID__
    if (config_.fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
#endif
    const auto shader_dir =
        std::filesystem::path(SDL_GetBasePath()) / TH2_ANIME4K_SHADER_DIR;
    upscaler_ = th2::create_upscaler(
        renderer_, shader_dir, config_.anime4k,
        &anime4k_available_);
    last_anime4k_wanted_ = config_.anime4k;
    imgui_ = std::make_unique<th2::ImGuiLayer>(window_, renderer_);
    SDL_Log("ImGui layer initialized");
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
    if (soak_directory) {
        soak_ = std::make_unique<th2::SoakGameDriver<Game>>(
            *this,
            *soak_directory, soak_runs);
        if (!soak_->start()) {
            running_ = false;
        }
    } else if (scenario) {
        reset_play_state();
        initialize_scenario_flags();
        direct_scenario_ = true;
        runtime_.load_file(*scenario);
        ui_mode_ = UiMode::game;
        advance();
    } else {
        SDL_Log("Starting opening movie");
        start_movie(3, 0, false);
    }
    SDL_Log("Game constructor finished");
}

Game::~Game()
{
    // All SDL resources are owned by members declared after
    // window_holder_/renderer_holder_, so they are destroyed before the
    // renderer/window and before SDL_Quit().  Only the config needs an
    // explicit teardown step.
    sync_window_config();
    th2::save_config(config_path_, config_);
}

int Game::run()
{
    try {
        return run_loop();
    } catch (const std::exception& error) {
        if (soak_) {
            soak_->fail(error.what());
        }
        throw;
    }
}

bool Game::is_confirm_key(SDL_Keycode key)
{
    return key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE;
}

bool Game::is_alt_enter(const SDL_KeyboardEvent& key)
{
    return (key.mod & SDL_KMOD_ALT) != 0
        && (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER);
}

int Game::run_loop()
{
    SDL_Log("Entering main loop");
    int dbg_w = 0, dbg_h = 0, dbg_pw = 0, dbg_ph = 0;
    SDL_GetWindowSize(window_, &dbg_w, &dbg_h);
    SDL_GetWindowSizeInPixels(window_, &dbg_pw, &dbg_ph);
    SDL_Log(
        "Window size: %dx%d (points), %dx%d (pixels), display scale %.2f",
        dbg_w, dbg_h, dbg_pw, dbg_ph, SDL_GetWindowDisplayScale(window_));
    constexpr auto frame_duration = std::chrono::nanoseconds(
        1'000'000'000 / 60);
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
            if (!gamepad_input_.process_event(event)) {
                continue;
            }
            if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                app_active_ = false;
                SDL_Log("App entered background");
                // Persist the system-save flags immediately. On Android the
                // process may be killed while in the background before the
                // destructor runs.
                sync_game_flags();
                continue;
            }
            if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                app_active_ = true;
                SDL_Log("App entered foreground");
                reset_render_state();
                continue;
            }
            if (event.type == SDL_EVENT_RENDER_TARGETS_RESET) {
                SDL_Log("Render targets reset");
                reset_render_state();
                continue;
            }
            if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                SDL_Log("Render device reset");
                reset_render_state();
                continue;
            }
            if (event.type == SDL_EVENT_RENDER_DEVICE_LOST) {
                SDL_Log("Render device lost");
                continue;
            }
#ifndef __ANDROID__
            if (event.type == SDL_EVENT_WINDOW_MOVED
                || event.type == SDL_EVENT_WINDOW_RESIZED
                || event.type == SDL_EVENT_WINDOW_RESTORED
                || event.type == SDL_EVENT_WINDOW_MAXIMIZED) {
                sync_window_config();
            }
#endif
            imgui_->process_event(event);
            touch_input_.process_event(event);
            if (event.type == SDL_EVENT_FINGER_DOWN) {
                imgui_->on_touch_down(
                    event.tfinger.x, event.tfinger.y);
            } else if (event.type == SDL_EVENT_FINGER_MOTION) {
                imgui_->on_touch_motion(
                    event.tfinger.x, event.tfinger.y,
                    event.tfinger.dx, event.tfinger.dy);
            } else if (event.type == SDL_EVENT_FINGER_UP) {
                imgui_->on_touch_up(
                    event.tfinger.x, event.tfinger.y);
            }
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
            if (event.type == SDL_EVENT_KEY_DOWN && is_alt_enter(event.key)) {
                toggle_fullscreen();
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
                        || is_confirm_key(event.key.key));
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
                    if (is_confirm_key(event.key.key)) {
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
                        toggle_fullscreen();
                    } else if (is_confirm_key(event.key.key)) {
                        manual_advance();
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    open_system_menu();
                } else if (event.button.button == SDL_BUTTON_LEFT) {
                    if (event.button.which != SDL_TOUCH_MOUSEID
                        && handle_sidebar_click(event.button.x, event.button.y)) {
                        suppress_sidebar_mouse_up_ = true;
                        continue;
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    if (suppress_sidebar_mouse_up_) {
                        suppress_sidebar_mouse_up_ = false;
                        backlog_handle_dragging_ = false;
                        continue;
                    }
                    if (choosing_) {
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
                            y += choice_height(choices_[i]);
                        }
                    } else {
                        manual_advance();
                    }
                }
                backlog_handle_dragging_ = false;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                if (event.wheel.y > 0
                    && config_.wheel_opens_backlog) {
                    open_backlog();
                } else if (event.wheel.y < 0) {
                    manual_advance();
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if (backlog_handle_dragging_) {
                    set_backlog_from_sidebar_y(event.motion.y);
                    continue;
                }
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
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
                       && event.button.button == SDL_BUTTON_LEFT) {
                backlog_handle_dragging_ = false;
            }
        }
        handle_touch_actions();
        process_save_bundle_dialog();

        if (!app_active_) {
            // Pause the loop while the app is in the background so we
            // don't keep rendering to a surface that may be destroyed.
            SDL_Delay(50);
            continue;
        }
        const bool control_held =
            (SDL_GetModState() & SDL_KMOD_CTRL) != 0
            || touch_input_.skip_held();
        if (movie_) {
            movie_->set_speed(control_held ? 4.0 : 1.0);
        } else if (control_held && ui_mode_ == UiMode::title) {
            title_started_ -= std::chrono::milliseconds(50);
            if (title_exit_started_) {
                *title_exit_started_ -= std::chrono::milliseconds(50);
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
        if (soak_) {
            soak_->step();
        }
        update_audio();
        update_movie();
        update_map();
        update_playback_modes();
        update_title();
        if (soak_) {
            update_transition();
            update_background_fade();
            update_screen_flash();
            update_shake();
            update_background_scroll();
            update_character_animations();
            update_clock_calendar();
            update_sakura();
            retire_soak_gpu_work();
            next_frame = std::chrono::steady_clock::now();
            continue;
        }
        ensure_upscaler();
        int output_width = 800;
        int output_height = 600;
        SDL_GetRenderOutputSize(
            renderer_, &output_width, &output_height);
        const float scale_x = output_width / 800.0f;
        const float scale_y = output_height / 600.0f;
        const float framebuffer_scale = std::min(scale_x, scale_y);
        const float display_scale = imgui_display_scale();
        imgui_->new_frame(window_, display_scale);
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
    SDL_Log("Main loop exited cleanly");
    return 0;
}

void Game::draw()
{
    draw_frame();
}

}  // namespace th2app

using th2app::Game;
using th2app::SdlSubsystem;
using th2app::discover_game_data_path;
using th2app::writable_directory;

int main(int argc, char** argv)
{
    try {
        SdlSubsystem sdl_subsystem;
#ifdef __ANDROID__
        std::filesystem::path data =
            std::filesystem::path(SDL_GetAndroidInternalStoragePath()) /
            "game-data";
#else
        std::filesystem::path data = "game-data";
#endif
        std::optional<std::filesystem::path> scenario;
        std::optional<std::filesystem::path> soak_directory;
        std::size_t soak_runs = 1;
        bool data_set = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--scenario") {
                if (++index >= argc) {
                    throw std::runtime_error("--scenario requires an SDT path");
                }
                scenario = argv[index];
            } else if (argument == "--soak") {
                soak_directory = writable_directory() / "logs" / "soak";
            } else if (argument == "--soak-state") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--soak-state requires a directory");
                }
                soak_directory = argv[index];
            } else if (argument == "--soak-runs") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--soak-runs requires a positive number");
                }
                soak_runs = std::stoull(argv[index]);
                if (soak_runs == 0) {
                    throw std::runtime_error(
                        "--soak-runs requires a positive number");
                }
            } else if (!data_set) {
                data = argv[index];
                data_set = true;
            } else {
                throw std::runtime_error(
                    "usage: toheart2 [GAME_DATA_DIRECTORY] "
                    "[--scenario FILE.SDT] [--soak] "
                    "[--soak-state DIRECTORY] [--soak-runs COUNT]");
            }
        }
        if (scenario && soak_directory) {
            throw std::runtime_error(
                "--scenario and --soak cannot be used together");
        }

#ifdef __ANDROID__
        // Pause SDL event processing while the app is backgrounded.  The
        // main loop still checks app_active_, but this keeps SDL from
        // presenting frames to a surface that may be destroyed on sleep.
        SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
        // Keep the Android back button as an SDL key event instead of letting
        // the OS finish the activity.
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        // We handle touch gestures ourselves; don't let SDL synthesize mouse
        // events from finger input, which otherwise causes a "tap" on release
        // to leave the backlog/Advance text.
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
        av_log_set_level(AV_LOG_ERROR);   // suppress warnings, keep errors
        auto discovered_data = discover_game_data_path(data, data_set);
        if (!discovered_data) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "Game data directory not found or invalid: %s",
                data.string().c_str());
            return 1;
        }
        data = *discovered_data;
        SDL_Log("Game data path: %s", data.string().c_str());
        SDL_Log("Game files found, starting engine");

        return Game(data, scenario, soak_directory, soak_runs).run();
    } catch (const std::exception& error) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Fatal error: %s", error.what());
        return 1;
    }
}
