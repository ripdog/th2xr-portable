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
    source.anime4k = false;
    source.authentic_font = true;
    source.sidebar_mode = 2;
    source.fullscreen = true;
    source.window_x = 123;
    source.window_y = 456;
    source.window_width = 1440;
    source.window_height = 900;
    source.font_family = "Test Sans";
    source.font_size = 31;
    source.character_voice_volume[3] = 17;
    source.character_voice_muted[3] = true;
    th2::save_config(path, source);

    const auto loaded = th2::load_config(path);
    std::filesystem::remove(path);
    if (loaded.bgm_volume != 99 || !loaded.bgm_muted
        || !loaded.auto_skip_read
        || loaded.anime4k
        || !loaded.authentic_font
        || loaded.sidebar_mode != 2
        || !loaded.fullscreen
        || loaded.window_x != 123
        || loaded.window_y != 456
        || loaded.window_width != 1440
        || loaded.window_height != 900
        || loaded.font_family != "Test Sans"
        || loaded.font_size != 31
        || loaded.character_voice_volume[3] != 17
        || !loaded.character_voice_muted[3]) {
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
