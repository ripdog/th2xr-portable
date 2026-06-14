#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace th2 {

struct CharacterState {
    int number = 0;
    int pose = 0;
    int locate = 1;
    int layer = 0;
    int brightness = 128;
    int alpha = 256;
};

std::string character_asset_name(int number, int pose);
int character_offset(int locate);

class Characters {
public:
    CharacterState& set(int number, int pose, int locate, int layer,
                        int brightness, int alpha);
    void remove(int number);
    void clear();
    CharacterState* find(int number);
    const CharacterState* find(int number) const;
    std::vector<CharacterState> ordered() const;

private:
    std::vector<CharacterState> characters_;
};

}  // namespace th2
