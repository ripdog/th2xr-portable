#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string_view>

namespace th2 {
namespace {

bool parse_bool(std::string_view value)
{
    return value == "1" || value == "true" || value == "yes";
}

int parse_int(std::string_view value, int fallback, int minimum, int maximum)
{
    try {
        return std::clamp(std::stoi(std::string(value)), minimum, maximum);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

GameConfig load_config(const std::filesystem::path& path)
{
    GameConfig config;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string_view key(line.data(), equal);
        const std::string_view value(
            line.data() + equal + 1, line.size() - equal - 1);
        if (key == "bgm_volume") {
            config.bgm_volume = parse_int(value, config.bgm_volume, 0, 256);
        } else if (key == "se_volume") {
            config.se_volume = parse_int(value, config.se_volume, 0, 256);
        } else if (key == "voice_volume") {
            config.voice_volume =
                parse_int(value, config.voice_volume, 0, 256);
        } else if (key == "bgm_muted") {
            config.bgm_muted = parse_bool(value);
        } else if (key == "se_muted") {
            config.se_muted = parse_bool(value);
        } else if (key == "voice_muted") {
            config.voice_muted = parse_bool(value);
        } else if (key == "auto_line_ms") {
            config.auto_line_ms =
                parse_int(value, config.auto_line_ms, 250, 10000);
        } else if (key == "auto_page_ms") {
            config.auto_page_ms =
                parse_int(value, config.auto_page_ms, 500, 15000);
        } else if (key == "text_speed_ms") {
            config.text_speed_ms =
                parse_int(value, config.text_speed_ms, 0, 100);
        } else if (key == "auto_skip_read") {
            config.auto_skip_read = parse_bool(value);
        } else if (key == "skip_unread") {
            config.skip_unread = parse_bool(value);
        } else if (key == "wheel_opens_backlog") {
            config.wheel_opens_backlog = parse_bool(value);
        } else if (key == "fullscreen") {
            config.fullscreen = parse_bool(value);
        } else if (key == "anime4k") {
            config.anime4k = parse_bool(value);
        } else if (key == "authentic_font") {
            config.authentic_font = parse_bool(value);
        } else if (key == "font_family") {
            config.font_family = value;
        } else if (key == "font_size") {
            config.font_size =
                parse_int(value, config.font_size, 12, 48);
        } else if (key == "show_script_position") {
            config.show_script_position = parse_bool(value);
        } else if (key == "dump_transition_frames") {
            config.dump_transition_frames = parse_bool(value);
        } else if (key == "name_family") {
            config.player_name.family = value;
        } else if (key == "name_given") {
            config.player_name.given = value;
        } else if (key == "name_family_reading") {
            config.player_name.family_reading = value;
        } else if (key == "name_given_reading") {
            config.player_name.given_reading = value;
        } else if (key == "name_nickname") {
            config.player_name.nickname = value;
        } else if (key == "name_nickname_reading") {
            config.player_name.nickname_reading = value;
        } else if (key.starts_with("character_voice_volume_")) {
            const auto index = parse_int(
                key.substr(std::string_view("character_voice_volume_").size()),
                -1, -1, 10);
            if (index >= 0) {
                config.character_voice_volume[index] =
                    parse_int(value, 256, 0, 256);
            }
        } else if (key.starts_with("character_voice_muted_")) {
            const auto index = parse_int(
                key.substr(std::string_view("character_voice_muted_").size()),
                -1, -1, 10);
            if (index >= 0) {
                config.character_voice_muted[index] = parse_bool(value);
            }
        } else if (key == "read") {
            config.read_lines.emplace(value);
        } else if (key.starts_with("game_flag_")) {
            const auto index = parse_int(
                key.substr(std::string_view("game_flag_").size()),
                -1, -1, static_cast<int>(config.game_flags.size() - 1));
            if (index >= 0) {
                config.game_flags[static_cast<std::size_t>(index)] =
                    parse_int(
                        value, 0,
                        std::numeric_limits<std::int32_t>::min(),
                        std::numeric_limits<std::int32_t>::max());
            }
        } else if (key == "visual_cg") {
            const auto cg = parse_int(value, -1, -1, 99999);
            if (cg >= 0) {
                config.unlocked_visual_cgs.emplace(cg);
            }
        } else if (key == "h_cg") {
            const auto cg = parse_int(value, -1, -1, 99999);
            if (cg >= 0) {
                config.unlocked_h_cgs.emplace(cg);
            }
        } else if (key == "replay") {
            const auto replay = parse_int(value, -1, -1, 255);
            if (replay >= 0) {
                config.unlocked_replays.emplace(replay);
            }
        }
    }
    return config;
}

void save_config(const std::filesystem::path& path, const GameConfig& config)
{
    std::ofstream output(path);
    output << "bgm_volume=" << config.bgm_volume << '\n'
           << "se_volume=" << config.se_volume << '\n'
           << "voice_volume=" << config.voice_volume << '\n'
           << "bgm_muted=" << config.bgm_muted << '\n'
           << "se_muted=" << config.se_muted << '\n'
           << "voice_muted=" << config.voice_muted << '\n'
           << "auto_line_ms=" << config.auto_line_ms << '\n'
           << "auto_page_ms=" << config.auto_page_ms << '\n'
           << "text_speed_ms=" << config.text_speed_ms << '\n'
           << "auto_skip_read=" << config.auto_skip_read << '\n'
           << "skip_unread=" << config.skip_unread << '\n'
           << "wheel_opens_backlog=" << config.wheel_opens_backlog << '\n'
           << "fullscreen=" << config.fullscreen << '\n'
           << "anime4k=" << config.anime4k << '\n'
           << "authentic_font=" << config.authentic_font << '\n'
           << "font_family=" << config.font_family << '\n'
           << "font_size=" << config.font_size << '\n'
           << "show_script_position=" << config.show_script_position << '\n'
           << "dump_transition_frames=" << config.dump_transition_frames << '\n'
           << "name_family=" << config.player_name.family << '\n'
           << "name_given=" << config.player_name.given << '\n'
           << "name_family_reading=" << config.player_name.family_reading << '\n'
           << "name_given_reading=" << config.player_name.given_reading << '\n'
           << "name_nickname=" << config.player_name.nickname << '\n'
           << "name_nickname_reading="
           << config.player_name.nickname_reading << '\n';
    for (std::size_t i = 0; i < config.character_voice_volume.size(); ++i) {
        output << "character_voice_volume_" << i << '='
               << config.character_voice_volume[i] << '\n'
               << "character_voice_muted_" << i << '='
               << config.character_voice_muted[i] << '\n';
    }
    for (const auto& key : config.read_lines) {
        output << "read=" << key << '\n';
    }
    for (std::size_t i = 0; i < config.game_flags.size(); ++i) {
        if (config.game_flags[i] != 0) {
            output << "game_flag_" << i << '=' << config.game_flags[i] << '\n';
        }
    }
    for (const auto cg : config.unlocked_visual_cgs) {
        output << "visual_cg=" << cg << '\n';
    }
    for (const auto cg : config.unlocked_h_cgs) {
        output << "h_cg=" << cg << '\n';
    }
    for (const auto replay : config.unlocked_replays) {
        output << "replay=" << replay << '\n';
    }
}

int auto_delay_ms(const GameConfig& config, bool text_is_read,
                  bool has_hidden_segments, bool message_ends_block)
{
    if (config.auto_skip_read && text_is_read) {
        return 40;
    }
    return !has_hidden_segments && message_ends_block
        ? config.auto_page_ms : config.auto_line_ms;
}

}  // namespace th2
