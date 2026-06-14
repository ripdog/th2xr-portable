#pragma once

#include "soak.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace th2 {

template <typename Game>
class SoakGameDriver {
public:
    SoakGameDriver(
        Game& game, const std::filesystem::path& directory,
        std::size_t run_limit)
        : game_(game),
          explorer_(std::make_unique<SoakExplorer>(directory, run_limit))
    {
        for (std::size_t i = 0; i < game_flags_.size(); ++i) {
            game_flags_[i] = game_.runtime_.game_flag(i);
        }
    }

    bool start()
    {
        if (!explorer_->begin_run()) {
            return false;
        }
        start_route();
        return true;
    }

    void finish_route(std::string script, std::size_t offset)
    {
        route_finished_ = true;
        terminal_script_ = std::move(script);
        terminal_offset_ = offset;
    }

    void fail(std::string_view error)
    {
        explorer_->fail_run(
            error, game_.runtime_.script_name(), game_.runtime_.vm_pc());
    }

    void step()
    {
        if (route_finished_) {
            explorer_->complete_run(
                "returned to title", terminal_script_, terminal_offset_);
            if (!explorer_->begin_run()) {
                std::cout << "soak complete: " << explorer_->completed_runs()
                          << " routes this run, " << explorer_->pending_paths()
                          << " pending; state in "
                          << explorer_->directory().string() << '\n';
                game_.running_ = false;
                return;
            }
            start_route();
            return;
        }

        const auto progress = progress_key();
        if (progress == last_progress_) {
            if (++stagnant_ticks_ > 10000) {
                throw std::runtime_error(
                    "soak stalled without engine progress");
            }
        } else {
            last_progress_ = progress;
            stagnant_ticks_ = 0;
        }

        if (game_.movie_) {
            game_.complete_movie();
            return;
        }
        if (game_.choosing_) {
            std::vector<SoakOption> options;
            options.reserve(game_.choices_.size());
            for (const auto& choice : game_.choices_) {
                options.push_back({choice.text, choice.sno});
            }
            game_.choice_selected_ = explorer_->select(
                SoakDecisionKind::choice,
                std::format(
                    "{}:{}", game_.runtime_.script_name(),
                    game_.runtime_.vm_pc()),
                options);
            game_.advance(true);
            return;
        }
        if (game_.ui_mode_ == Game::UiMode::map) {
            if (game_.map_fade_ticks_ != 0
                || game_.map_slide_ticks_ != 0) {
                game_.map_tick_ -= std::chrono::seconds(1);
                return;
            }
            std::vector<SoakOption> options;
            options.reserve(game_.map_events_.size());
            for (const auto& event : game_.map_events_) {
                options.push_back({
                    std::format(
                        "character={} position={} type={}",
                        event.character, event.position, event.type),
                    event.script,
                });
            }
            const int selected = explorer_->select(
                SoakDecisionKind::map,
                std::format(
                    "{}:{}", game_.runtime_.script_name(),
                    game_.runtime_.vm_pc()),
                options);
            game_.finish_map_selection(selected);
            game_.map_tick_ -= std::chrono::seconds(1);
            return;
        }
        game_.skip(true);
    }

private:
    Game& game_;
    std::unique_ptr<SoakExplorer> explorer_;
    std::array<std::int32_t, 1024> game_flags_{};
    bool route_finished_ = false;
    std::string terminal_script_;
    std::size_t terminal_offset_ = 0;
    std::string last_progress_;
    std::size_t stagnant_ticks_ = 0;

    void start_route()
    {
        for (std::size_t i = 0; i < game_flags_.size(); ++i) {
            game_.runtime_.set_game_flag(i, game_flags_[i]);
            game_.config_.game_flags[i] = game_flags_[i];
        }
        route_finished_ = false;
        terminal_script_.clear();
        terminal_offset_ = 0;
        last_progress_.clear();
        stagnant_ticks_ = 0;
        game_.start_new_game();
    }

    std::string progress_key() const
    {
        return std::format(
            "{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            game_.runtime_.script_name(), game_.runtime_.vm_pc(),
            static_cast<int>(game_.ui_mode_), game_.waiting_for_input_,
            game_.choosing_, game_.movie_ != nullptr,
            game_.transition_.has_value(),
            game_.background_fade_.has_value(),
            game_.audio_wait_.has_value(), game_.map_fade_ticks_);
    }
};

}  // namespace th2
