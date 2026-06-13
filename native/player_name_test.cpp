#include "player_name.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    const th2::PlayerName name{
        "Smith", "Alex", "smith", "alex", "Al", "AL",
    };
    if (th2::substitute_player_name(
            "*nf *nl (*nn) *nf2/*nl3/*nnk1", name)
        != "Alex Smith (Al) l/i/A") {
        return 1;
    }
    const auto path =
        std::filesystem::temp_directory_path() / "th2-name-defaults.exe";
    {
        std::ofstream output(path, std::ios::binary);
        for (const char* value :
             {"Kouno", "Kouno", "Takaaki", "Takaaki", "Taka", "Taka"}) {
            output.write(value, static_cast<std::streamsize>(
                std::char_traits<char>::length(value)));
            for (std::size_t i = std::char_traits<char>::length(value);
                 i < 16; ++i) {
                output.put('\0');
            }
        }
    }
    const auto defaults = th2::load_default_player_name(path);
    std::filesystem::remove(path);
    if (defaults.family != "Kouno" || defaults.given != "Takaaki"
        || defaults.nickname != "Taka") {
        return 2;
    }
    auto changed = defaults;
    changed.nickname_reading = "TAKA";
    if (!th2::uses_default_voice_name(changed, defaults)) {
        return 3;
    }
    changed.nickname = "Tak";
    if (th2::uses_default_voice_name(changed, defaults)) {
        return 4;
    }
    if (argc > 1) {
        const auto patched = th2::load_default_player_name(argv[1]);
        if (patched.family != "Kouno" || patched.given != "Takaaki"
            || patched.nickname != "Taka") {
            std::cerr << patched.family << '/' << patched.given << '/'
                      << patched.nickname << '\n';
            return 5;
        }
    }
    return 0;
}
