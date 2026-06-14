#include "player_name.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>

namespace th2 {
namespace {

std::string indexed_value(std::string_view value, char index)
{
    if (index < '1' || index > '6') {
        return std::string(value);
    }
    const auto wanted = static_cast<std::size_t>(index - '1');
    std::size_t position = 0;
    for (std::size_t character = 0; position < value.size(); ++character) {
        const auto start = position;
        const auto lead = static_cast<unsigned char>(value[position++]);
        if ((lead & 0x80) != 0) {
            int continuation = (lead & 0xe0) == 0xc0 ? 1
                : (lead & 0xf0) == 0xe0 ? 2
                : (lead & 0xf8) == 0xf0 ? 3 : 0;
            while (continuation-- > 0 && position < value.size()) {
                ++position;
            }
        }
        if (character == wanted) {
            return std::string(value.substr(start, position - start));
        }
    }
    return {};
}

}  // namespace

PlayerName load_default_player_name(const std::filesystem::path& executable)
{
    PlayerName result{
        "河野", "貴明", "こうの", "たかあき", "たか", "タカ",
    };
    std::ifstream input(executable, std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto read_slot = [&](std::size_t offset) {
        const auto end = bytes.find('\0', offset);
        if (end == std::string::npos || end - offset > 12) {
            return std::string{};
        }
        return bytes.substr(offset, end - offset);
    };
    const auto translated_name = [](const std::string& value) {
        return value.size() >= 3
            && std::all_of(
                value.begin(), value.end(),
                [](unsigned char byte) { return std::isalpha(byte); });
    };
    for (std::size_t offset = 0; offset + 96 <= bytes.size(); offset += 16) {
        const std::array values{
            read_slot(offset),
            read_slot(offset + 16),
            read_slot(offset + 32),
            read_slot(offset + 48),
            read_slot(offset + 64),
            read_slot(offset + 80),
        };
        if (translated_name(values[0]) && values[0] == values[1]
            && translated_name(values[2]) && values[2] == values[3]
            && translated_name(values[4]) && values[4] == values[5]
            && values[0] != values[2] && values[2] != values[4]) {
            result = {
                values[0], values[2], values[1],
                values[3], values[4], values[5],
            };
        }
    }
    return result;
}

bool uses_default_voice_name(
    const PlayerName& name, const PlayerName& default_name)
{
    return name.family == default_name.family
        && name.given == default_name.given
        && name.family_reading == default_name.family_reading
        && name.given_reading == default_name.given_reading
        && name.nickname == default_name.nickname;
}

std::string substitute_player_name(
    std::string_view source, const PlayerName& name,
    bool use_komaki_given_name)
{
    struct Replacement {
        std::string_view token;
        const std::string* value;
    };
    const std::array replacements{
        Replacement{"*nnk", &name.nickname_reading},
        Replacement{"*nlk", &name.family_reading},
        Replacement{"*nfk", &name.given_reading},
        Replacement{"*nn", &name.nickname},
        Replacement{"*nl", &name.family},
        Replacement{"*nf", &name.given},
    };

    std::string result;
    for (std::size_t position = 0; position < source.size();) {
        if (source.substr(position).starts_with("*h2")) {
            result += use_komaki_given_name ? "Manaka" : "Komaki";
            position += 3;
            continue;
        }
        bool replaced = false;
        for (const auto& replacement : replacements) {
            if (source.substr(position).starts_with(replacement.token)) {
                position += replacement.token.size();
                char index = '\0';
                if (position < source.size()
                    && source[position] >= '1' && source[position] <= '6') {
                    index = source[position++];
                }
                result += indexed_value(*replacement.value, index);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.push_back(source[position++]);
        }
    }
    return result;
}

}  // namespace th2
