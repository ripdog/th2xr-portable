#include "event.hpp"

#include "event_metadata.h"
#include "expression.hpp"

#include <cerrno>
#include <iconv.h>
#include <stdexcept>
#include <string>

namespace th2 {
namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t position)
{
    return static_cast<std::uint16_t>(bytes[position])
        | (static_cast<std::uint16_t>(bytes[position + 1]) << 8);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t position)
{
    return static_cast<std::uint32_t>(bytes[position])
        | (static_cast<std::uint32_t>(bytes[position + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[position + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[position + 3]) << 24);
}

std::int32_t number(
    std::span<const std::uint8_t> bytes, std::size_t& position,
    std::span<const std::int32_t> registers)
{
    const auto mode = bytes[position++];
    if (mode == 2) {
        const auto length = bytes[position++];
        const std::string expression(
            reinterpret_cast<const char*>(bytes.data() + position), length);
        position += length;
        return evaluate_expression(expression, registers);
    }
    const auto raw = static_cast<std::int32_t>(read_u32(bytes, position));
    position += 4;
    if (mode != 0) {
        return raw;
    }
    if (raw < 0 || static_cast<std::size_t>(raw) >= registers.size()) {
        throw std::runtime_error("event register index is out of range");
    }
    return registers[raw];
}

std::string decode_script_text(
    const std::uint8_t* bytes, std::size_t length)
{
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        throw std::runtime_error("CP932 converter is unavailable");
    }
    std::string output(length * 3 + 1, '\0');
    char* input = reinterpret_cast<char*>(
        const_cast<std::uint8_t*>(bytes));
    std::size_t input_left = length;
    char* destination = output.data();
    std::size_t output_left = output.size();
    const auto result = iconv(
        converter, &input, &input_left, &destination, &output_left);
    iconv_close(converter);
    if (result == static_cast<std::size_t>(-1) || input_left != 0) {
        throw std::runtime_error("invalid CP932 script text");
    }
    output.resize(output.size() - output_left);
    return output;
}

}  // namespace

Event decode_event(
    const Instruction& instruction,
    std::span<const std::uint8_t> bytes,
    std::span<const std::int32_t> registers)
{
    if (instruction.opcode < 64 || bytes.size() != instruction.size) {
        throw std::runtime_error("invalid event instruction");
    }
    const char* parameters = th2_event_parameters(instruction.opcode);
    if (!parameters) {
        throw std::runtime_error("unknown event opcode");
    }

    Event event{instruction, {}};
    std::size_t position = 2;
    for (std::size_t index = 0; index < 15; ++index) {
        switch (parameters[index]) {
        case TH2_PARAMETER_END:
            return event;
        case TH2_PARAMETER_BYTE:
        case TH2_PARAMETER_ADD:
            event.arguments.emplace_back(static_cast<std::int32_t>(bytes[position++]));
            break;
        case TH2_PARAMETER_COUNT:
        case TH2_PARAMETER_VOICE_COUNT:
            event.arguments.emplace_back(static_cast<std::int32_t>(
                read_u16(bytes, position)));
            position += 2;
            break;
        case TH2_PARAMETER_NUMBER:
            event.arguments.emplace_back(number(bytes, position, registers));
            break;
        case TH2_PARAMETER_STRING8: {
            const auto length = bytes[position++];
            event.arguments.emplace_back(
                decode_script_text(bytes.data() + position, length));
            position += length;
            break;
        }
        case TH2_PARAMETER_STRING16: {
            const auto length = read_u16(bytes, position);
            position += 2;
            event.arguments.emplace_back(
                decode_script_text(bytes.data() + position, length));
            position += length;
            break;
        }
        case TH2_PARAMETER_REGISTER:
            event.arguments.emplace_back(RegisterTarget{bytes[position++]});
            break;
        case TH2_PARAMETER_COMPARE: {
            const auto register_index = bytes[position++];
            const auto operation = bytes[position++];
            event.arguments.emplace_back(Comparison{
                register_index, operation, number(bytes, position, registers)});
            break;
        }
        default:
            throw std::runtime_error("unknown event parameter type");
        }
    }
    return event;
}

}  // namespace th2
