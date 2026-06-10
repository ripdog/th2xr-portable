#include "character.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>

namespace th2 {

std::string character_asset_name(int number, int pose)
{
    return std::format("c{:03d}{:05d}.tga", number, pose);
}

int character_offset(int locate)
{
    static constexpr std::array offsets{
        -200, 0, 200, -240, 240, -375, 375, -500, 500,
    };
    if (locate < 0 || static_cast<std::size_t>(locate) >= offsets.size()) {
        throw std::out_of_range("invalid character location");
    }
    return offsets[locate];
}

CharacterState& Characters::set(int number, int pose, int locate, int layer,
                                int brightness, int alpha)
{
    auto* character = find(number);
    if (!character) {
        characters_.push_back(CharacterState{});
        character = &characters_.back();
        character->number = number;
    }
    character->pose = pose;
    character->locate = locate;
    character->layer = layer;
    character->brightness = brightness;
    character->alpha = alpha;
    return *character;
}

void Characters::remove(int number)
{
    std::erase_if(characters_, [number](const CharacterState& character) {
        return character.number == number;
    });
}

void Characters::clear()
{
    characters_.clear();
}

CharacterState* Characters::find(int number)
{
    const auto found = std::find_if(
        characters_.begin(), characters_.end(),
        [number](const CharacterState& character) {
            return character.number == number;
        });
    return found == characters_.end() ? nullptr : &*found;
}

const CharacterState* Characters::find(int number) const
{
    const auto found = std::find_if(
        characters_.begin(), characters_.end(),
        [number](const CharacterState& character) {
            return character.number == number;
        });
    return found == characters_.end() ? nullptr : &*found;
}

std::vector<CharacterState> Characters::ordered() const
{
    auto result = characters_;
    std::stable_sort(
        result.begin(), result.end(),
        [](const CharacterState& left, const CharacterState& right) {
            return left.layer < right.layer;
        });
    return result;
}

}  // namespace th2
