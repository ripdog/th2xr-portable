#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace th2 {

struct GameConfig {
    int bgm_volume = 176;
    int se_volume = 256;
    int voice_volume = 256;
    std::array<int, 11> character_voice_volume{256, 256, 256, 256, 256, 256,
                                                256, 256, 256, 256, 256};
    int auto_line_ms = 2000;
    int auto_page_ms = 4000;
    int text_speed_ms = 24;
    bool auto_skip_read = false;
    bool skip_unread = false;
    bool wheel_opens_backlog = true;
    bool fullscreen = false;
    std::unordered_set<std::string> read_lines;
};

GameConfig load_config(const std::filesystem::path& path);
void save_config(const std::filesystem::path& path, const GameConfig& config);
int auto_delay_ms(const GameConfig& config, bool text_is_read,
                  bool has_hidden_segments, bool message_ends_block);

}  // namespace th2
