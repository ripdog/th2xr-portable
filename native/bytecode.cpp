#include "bytecode.hpp"

#include "event_metadata.h"

#include <array>
#include <stdexcept>
#include <string>

namespace th2 {
namespace {

constexpr std::uint16_t core_opcode_end = 42;

constexpr std::array<std::string_view, core_opcode_end> core_names{
    "Start", "End", "MovR", "MovV", "Swap", "Rand", "IfR", "IfV",
    "IfElseR", "IfElseV", "Loop", "Goto", "Inc", "Dec", "Not", "Neg",
    "AddR", "AddV", "SubR", "SubV", "MulR", "MulV", "DivR", "DivV",
    "ModR", "ModV", "AndR", "AndV", "OrR", "OrV", "XorR", "XorV",
    "Calc", "Pusha", "Popa", "Call", "Ret", "Wait", "TWait", "Run",
    "SLoad", "TStart",
};

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t position)
{
    if (position + 2 > bytes.size()) {
        throw std::runtime_error("truncated 16-bit bytecode value");
    }
    return static_cast<std::uint16_t>(bytes[position])
        | (static_cast<std::uint16_t>(bytes[position + 1]) << 8);
}

void require(std::span<const std::uint8_t> bytes, std::size_t position, std::size_t count)
{
    if (position > bytes.size() || count > bytes.size() - position) {
        throw std::runtime_error("truncated instruction");
    }
}

std::size_t number_size(std::span<const std::uint8_t> bytes, std::size_t position)
{
    require(bytes, position, 1);
    if (bytes[position] != 2) {
        require(bytes, position, 5);
        return 5;
    }
    require(bytes, position, 2);
    const auto length = bytes[position + 1];
    require(bytes, position, 2 + length);
    return 2 + length;
}

std::size_t event_size(
    std::span<const std::uint8_t> bytes, std::size_t offset, std::uint16_t opcode)
{
    const char* parameters = th2_event_parameters(opcode);
    if (!parameters) {
        throw std::runtime_error("unknown event opcode " + std::to_string(opcode));
    }

    std::size_t position = offset + 2;
    for (std::size_t index = 0; index < 15; ++index) {
        switch (parameters[index]) {
        case TH2_PARAMETER_END:
            return position - offset;
        case TH2_PARAMETER_BYTE:
        case TH2_PARAMETER_REGISTER:
        case TH2_PARAMETER_ADD:
            require(bytes, position, 1);
            position += 1;
            break;
        case TH2_PARAMETER_COUNT:
        case TH2_PARAMETER_VOICE_COUNT:
            require(bytes, position, 2);
            position += 2;
            break;
        case TH2_PARAMETER_NUMBER:
            position += number_size(bytes, position);
            break;
        case TH2_PARAMETER_STRING8:
            require(bytes, position, 1);
            require(bytes, position + 1, bytes[position]);
            position += 1 + bytes[position];
            break;
        case TH2_PARAMETER_STRING16: {
            const auto length = read_u16(bytes, position);
            require(bytes, position + 2, length);
            position += 2 + length;
            break;
        }
        case TH2_PARAMETER_COMPARE:
            require(bytes, position, 2);
            position += 2;
            position += number_size(bytes, position);
            break;
        default:
            throw std::runtime_error("unknown event parameter type");
        }
    }
    return position - offset;
}

std::size_t core_size(
    std::span<const std::uint8_t> bytes, std::size_t offset, std::uint16_t opcode)
{
    static constexpr std::array<std::uint8_t, core_opcode_end> fixed_sizes{
        2, 2, 4, 7, 4, 3, 9, 12, 13, 16, 7, 6, 3, 3, 3, 3,
        4, 7, 4, 7, 4, 7, 4, 7, 4, 7, 4, 7, 4, 7, 4, 7,
        0, 2, 2, 3, 2, 4, 4, 2, 0, 2,
    };
    if (opcode == 32) {
        const auto expression_size = read_u16(bytes, offset + 3);
        require(bytes, offset, 5 + expression_size);
        return 5 + expression_size;
    }
    if (opcode == 40) {
        require(bytes, offset + 2, 1);
        const auto length = bytes[offset + 2];
        require(bytes, offset, 3 + length);
        return 3 + length;
    }
    const auto size = fixed_sizes[opcode];
    require(bytes, offset, size);
    return size;
}

}  // namespace

Instruction decode_instruction(std::span<const std::uint8_t> bytecode, std::size_t offset)
{
    const auto opcode = read_u16(bytecode, offset);
    if (opcode < core_opcode_end) {
        return {opcode, offset, core_size(bytecode, offset, opcode),
                core_names[opcode], opcode == 37 || opcode == 38 || opcode == 39
                    || opcode == 40};
    }

    const char* name = th2_event_name(opcode);
    if (!name) {
        throw std::runtime_error("unknown opcode " + std::to_string(opcode));
    }
    return {opcode, offset, event_size(bytecode, offset, opcode),
            name, th2_event_waits(opcode) != 0};
}

}  // namespace th2
