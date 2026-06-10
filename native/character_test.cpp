#include "character.hpp"

int main()
{
    if (th2::character_asset_name(1, 3040) != "c00103040.tga"
        || th2::character_offset(0) != -200
        || th2::character_offset(1) != 0
        || th2::character_offset(8) != 500) {
        return 1;
    }

    th2::Characters characters;
    characters.set(1, 3040, 1, 2, 128, 256);
    characters.set(2, 1000, 2, -1, 96, 128);
    characters.set(1, 3010, 0, 2, 120, 200);
    const auto* first = characters.find(1);
    if (!first || first->pose != 3010 || first->locate != 0
        || first->brightness != 120 || first->alpha != 200) {
        return 2;
    }
    const auto ordered = characters.ordered();
    if (ordered.size() != 2 || ordered[0].number != 2
        || ordered[1].number != 1) {
        return 3;
    }
    characters.remove(1);
    if (characters.find(1) || !characters.find(2)) {
        return 4;
    }
    characters.clear();
    if (!characters.ordered().empty()) {
        return 5;
    }
}
