#include "config.hpp"

#include <filesystem>

int main()
{
    const auto path =
        std::filesystem::temp_directory_path() / "th2-config-test.ini";
    th2::GameConfig source;
    source.bgm_volume = 99;
    source.bgm_muted = true;
    source.auto_skip_read = true;
    source.character_voice_volume[3] = 17;
    source.character_voice_muted[3] = true;
    source.player_name.family = "Smith";
    source.read_lines.insert("010301000.sdt:42");
    th2::save_config(path, source);

    const auto loaded = th2::load_config(path);
    std::filesystem::remove(path);
    if (loaded.bgm_volume != 99 || !loaded.bgm_muted
        || !loaded.auto_skip_read
        || loaded.character_voice_volume[3] != 17
        || !loaded.character_voice_muted[3]
        || loaded.player_name.family != "Smith"
        || !loaded.read_lines.contains("010301000.sdt:42")) {
        return 1;
    }
    if (th2::auto_delay_ms(loaded, false, true, true)
            != loaded.auto_line_ms
        || th2::auto_delay_ms(loaded, false, false, true)
            != loaded.auto_page_ms
        || th2::auto_delay_ms(loaded, true, false, true) != 40) {
        return 2;
    }
    return 0;
}
