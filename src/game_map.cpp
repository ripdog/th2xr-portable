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

std::string Game::current_read_key() const
{
    if (current_line_key_.empty() || message_.empty()) {
        return {};
    }
    return current_line_key_ + ':'
        + std::to_string(message_.revealed_count());
}

bool Game::current_text_is_read() const
{
    if (replay_mode_) {
        return true;
    }
    const auto key = current_read_key();
    return !key.empty() && config_.read_lines.contains(key);
}

void Game::mark_current_text_read()
{
    if (replay_mode_) {
        return;
    }
    const auto key = current_read_key();
    if (!key.empty() && config_.read_lines.insert(key).second) {
        th2::save_config(config_path_, config_);
    }
}

void Game::manual_advance()
{
    auto_mode_ = false;
    skip_mode_ = false;
    auto_next_time_.reset();
    advance();
}

int Game::map_sakura_type() const
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

std::uint16_t Game::map_u16(
    std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t Game::map_u32(
    std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | static_cast<std::uint32_t>(bytes[offset + 1]) << 8
        | static_cast<std::uint32_t>(bytes[offset + 2]) << 16
        | static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

Game::MapCharacter Game::load_sprite_animation(const std::string& stem)
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

Game::MapCharacter Game::load_map_character(const Game::MapEvent& event)
{
    return load_sprite_animation(std::format(
        "mapc{:02d}{}", event.character, event.type));
}

int Game::weekday(int month, int day) const
{
    if (month == 3) return day % 7;
    if (month == 4) return (day + 3) % 7;
    if (month == 5) return (day + 5) % 7;
    return 0;
}

int Game::calendar_holiday(int month, int day) const
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

void Game::begin_clock(int requested)
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

void Game::begin_calendar(int month, int day)
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

void Game::update_clock_calendar()
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

void Game::draw_sprite_frame(
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

void Game::draw_clock_calendar()
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

Texture Game::load_sakura_texture(std::string_view name)
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

void Game::start_sakura(int amount, bool no_reset)
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

void Game::stop_sakura(bool force)
{
    if (sakura_ && (force || !sakura_->no_reset)
        && sakura_->reset_frames < 0) {
        sakura_->no_reset = false;
        sakura_->reset_frames = 0;
    }
}

int Game::seasonal_background_scene(int scene) const
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

void Game::update_background_sakura(int scene, bool background)
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

std::uint32_t Game::next_sakura_random()
{
    sakura_random_ = sakura_random_ * 1103515245u + 12345u;
    return sakura_random_;
}

void Game::spawn_sakura_petals()
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

void Game::update_sakura()
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

void Game::draw_sakura()
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

std::string Game::map_field_name(int field) const
{
    const int variant = field == 1 || field == 4
        ? map_sakura_type() : 0;
    return std::format("map1{}{}.tga", field, variant);
}

void Game::begin_map()
{
    if (std::ranges::none_of(
            map_events_, [](const Game::MapEvent& event) {
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

void Game::finish_map_selection(int selected)
{
    map_selected_ = selected;
    map_fade_ticks_ = 16;
    play_se(-1, 9014, false, 255);
}

void Game::complete_map_selection()
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


}  // namespace th2app
