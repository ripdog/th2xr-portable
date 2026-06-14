#include "bytecode.hpp"
#include "event.hpp"
#include "event_metadata.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

int main()
{
    std::uint16_t opcode = 64;
    while (opcode < 64 + th2_event_count()
           && std::strcmp(th2_event_name(opcode), "LoadScript") != 0) {
        ++opcode;
    }
    const std::vector<std::uint8_t> bytes{
        static_cast<std::uint8_t>(opcode),
        static_cast<std::uint8_t>(opcode >> 8),
        4, 't', 'e', 's', 't',
    };
    const auto instruction = th2::decode_instruction(bytes, 0);
    const std::array<std::int32_t, 50> registers{};
    const auto event = th2::decode_event(instruction, bytes, registers);
    if (event.instruction.name != "LoadScript" || event.arguments.size() != 1
        || std::get<std::string>(event.arguments[0]) != "test") {
        return 1;
    }

    const std::vector<std::uint8_t> cp932_bytes{
        static_cast<std::uint8_t>(opcode),
        static_cast<std::uint8_t>(opcode >> 8),
        12, 0x81, 0x67, 'l', 'a', 't', 'e', 'n', 'e', 's', 's', 0x81, 0x68,
    };
    const auto cp932_instruction =
        th2::decode_instruction(cp932_bytes, 0);
    const auto cp932_event =
        th2::decode_event(cp932_instruction, cp932_bytes, registers);
    if (std::get<std::string>(cp932_event.arguments[0])
        != "\xe2\x80\x9clateness\xe2\x80\x9d") {
        return 2;
    }
}
