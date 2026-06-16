#include "icon.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: icon-test TOHEART2.EXE\n";
        return 2;
    }
    const auto icon = th2::load_executable_icon(std::filesystem::path(argv[1]));
    if (!icon) {
        return 4;
    }
    if (icon->w != 32 || icon->h != 32) {
        return 5;
    }
    return 0;
}
