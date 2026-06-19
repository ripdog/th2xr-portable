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

void Game::handle_title_input(const SDL_Event& event)
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

void Game::activate_cg_gallery_item()
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

void Game::handle_cg_gallery_input(const SDL_Event& event)
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

void Game::activate_music_room_item()
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

void Game::handle_music_room_input(const SDL_Event& event)
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

void Game::start_replay(int slot)
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

void Game::activate_replay_gallery_item()
{
    if (omake_highlight_ == 14) {
        play_se(-1, 9107, false, 255);
        close_omake_screen();
        return;
    }
    if (omake_highlight_ < 0 || omake_highlight_ >= 9
        || !unlocked_replays_.contains(
            replay_flags[omake_highlight_])) {
        return;
    }
    play_se(-1, 9104, false, 255);
    start_replay(omake_highlight_);
}

void Game::handle_replay_gallery_input(const SDL_Event& event)
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


}  // namespace th2app
