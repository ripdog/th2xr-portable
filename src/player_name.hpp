#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace th2 {

struct PlayerName {
    std::string family;
    std::string given;
    std::string family_reading;
    std::string given_reading;
    std::string nickname;
    std::string nickname_reading;
};

PlayerName load_default_player_name(const std::filesystem::path& executable);
bool uses_default_voice_name(
    const PlayerName& name, const PlayerName& default_name);
std::string substitute_player_name(
    std::string_view source, const PlayerName& name);

}  // namespace th2
