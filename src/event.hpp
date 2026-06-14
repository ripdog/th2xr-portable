#pragma once

#include "bytecode.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace th2 {

struct RegisterTarget {
    std::uint8_t index;
};

struct Comparison {
    std::uint8_t register_index;
    std::uint8_t operation;
    std::int32_t value;
};

using EventArgument = std::variant<std::int32_t, std::string, RegisterTarget, Comparison>;

struct Event {
    Instruction instruction;
    std::vector<EventArgument> arguments;
};

Event decode_event(
    const Instruction& instruction,
    std::span<const std::uint8_t> bytes,
    std::span<const std::int32_t> registers);

}  // namespace th2
