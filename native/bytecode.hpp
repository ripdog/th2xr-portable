#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace th2 {

struct Instruction {
    std::uint16_t opcode;
    std::size_t offset;
    std::size_t size;
    std::string_view name;
    bool waits;
};

Instruction decode_instruction(std::span<const std::uint8_t> bytecode, std::size_t offset);

}  // namespace th2
