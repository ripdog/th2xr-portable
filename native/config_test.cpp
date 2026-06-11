#include "config.hpp"

#include <filesystem>

int main()
{
    const auto path =
        std::filesystem::temp_directory_path() / "th2-config-test.ini";
    th2::GameConfig source;
    source.bgm_volume = 99;
    source.auto_skip_read = true;
    source.character_voice_volume[3] = 17;
    source.read_lines.insert("010301000.sdt:42");
    th2::save_config(path, source);

    const auto loaded = th2::load_config(path);
    std::filesystem::remove(path);
    if (loaded.bgm_volume != 99 || !loaded.auto_skip_read
        || loaded.character_voice_volume[3] != 17
        || !loaded.read_lines.contains("010301000.sdt:42")) {
        return 1;
    }
    return 0;
}
