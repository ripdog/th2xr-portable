#include "event.hpp"
#include "event_metadata.h"
#include "script_runtime.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::uint16_t find_opcode(const char* name)
{
    std::uint16_t opcode = 64;
    while (opcode < 64 + th2_event_count()
           && std::strcmp(th2_event_name(opcode), name) != 0) {
        ++opcode;
    }
    if (opcode >= 64 + th2_event_count()) {
        throw std::runtime_error(
            std::string("opcode not found: ") + name);
    }
    return opcode;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    const auto position = bytes.size();
    bytes.resize(position + 4);
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[position + shift / 8] =
            static_cast<std::uint8_t>(value >> shift);
    }
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t position,
               std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[position + shift / 8] =
            static_cast<std::uint8_t>(value >> shift);
    }
}

constexpr std::size_t scenario_header_size = 1032;

}  // namespace

int main()
{
    // --- Test 1: SetSelectMes event decoding ---
    {
        const auto opcode = find_opcode("SetSelectMes");
        if (!th2_event_waits(opcode)) {
            // SetSelectMes is a NOWAIT event
        } else {
            return 1;  // Wrong wait type
        }

        std::vector<std::uint8_t> bytes{
            static_cast<std::uint8_t>(opcode),
            static_cast<std::uint8_t>(opcode >> 8),
        };
        // STRING8 argument: "Choice A" (8 chars)
        const std::string text = "Choice A";
        bytes.push_back(static_cast<std::uint8_t>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
        // NUM argument 1 (flag_no): mode=1 (immediate), value=-1
        bytes.push_back(1);
        append_u32(bytes, static_cast<std::uint32_t>(-1));
        // NUM argument 2 (flag_value): mode=1, value=0
        bytes.push_back(1);
        append_u32(bytes, 0);
        // NUM argument 3 (mes_wait): mode=1, value=100
        bytes.push_back(1);
        append_u32(bytes, 100);

        const auto instruction = th2::decode_instruction(bytes, 0);
        const std::array<std::int32_t, 50> registers{};
        const auto event =
            th2::decode_event(instruction, bytes, registers);

        if (event.instruction.name != "SetSelectMes") {
            return 2;
        }
        if (event.arguments.size() != 4) {
            return 3;
        }
        if (std::get<std::string>(event.arguments[0]) != "Choice A") {
            return 4;
        }
        if (std::get<std::int32_t>(event.arguments[1]) != -1) {
            return 5;
        }
        if (std::get<std::int32_t>(event.arguments[2]) != 0) {
            return 6;
        }
        if (std::get<std::int32_t>(event.arguments[3]) != 100) {
            return 7;
        }
    }

    // --- Test 2: SetSelect event decoding ---
    {
        const auto opcode = find_opcode("SetSelect");
        if (!th2_event_waits(opcode)) {
            return 8;
        }

        std::vector<std::uint8_t> bytes{
            static_cast<std::uint8_t>(opcode),
            static_cast<std::uint8_t>(opcode >> 8),
        };
        // REG argument: register index 5
        bytes.push_back(5);

        const auto instruction = th2::decode_instruction(bytes, 0);
        const std::array<std::int32_t, 50> registers{};
        const auto event =
            th2::decode_event(instruction, bytes, registers);

        if (event.instruction.name != "SetSelect") {
            return 9;
        }
        if (event.arguments.size() != 1) {
            return 10;
        }
        const auto& target =
            std::get<th2::RegisterTarget>(event.arguments[0]);
        if (target.index != 5) {
            return 11;
        }
    }

    // --- Test 3: SetSelectMesEx event decoding ---
    {
        const auto opcode = find_opcode("SetSelectMesEx");
        if (th2_event_waits(opcode)) {
            return 12;  // Should be NOWAIT
        }

        std::vector<std::uint8_t> bytes{
            static_cast<std::uint8_t>(opcode),
            static_cast<std::uint8_t>(opcode >> 8),
        };
        // STRING8: "Choice X" (8 chars)
        const std::string text = "Choice X";
        bytes.push_back(static_cast<std::uint8_t>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
        // STRING8: "SC01" (4 chars, script filename)
        const std::string sno = "SC01";
        bytes.push_back(static_cast<std::uint8_t>(sno.size()));
        bytes.insert(bytes.end(), sno.begin(), sno.end());
        // NUM: flag_no=-1
        bytes.push_back(1);
        append_u32(bytes, static_cast<std::uint32_t>(-1));
        // NUM: flag_value=0
        bytes.push_back(1);
        append_u32(bytes, 0);

        const auto instruction = th2::decode_instruction(bytes, 0);
        const std::array<std::int32_t, 50> registers{};
        const auto event =
            th2::decode_event(instruction, bytes, registers);

        if (event.instruction.name != "SetSelectMesEx") {
            return 13;
        }
        if (event.arguments.size() != 4) {
            return 14;
        }
        if (std::get<std::string>(event.arguments[0]) != "Choice X") {
            return 15;
        }
        if (std::get<std::string>(event.arguments[1]) != "SC01") {
            return 16;
        }
        if (std::get<std::int32_t>(event.arguments[2]) != -1) {
            return 17;
        }
        if (std::get<std::int32_t>(event.arguments[3]) != 0) {
            return 18;
        }
    }

    // --- Test 4: SetSelectEx event decoding ---
    {
        const auto opcode = find_opcode("SetSelectEx");
        if (!th2_event_waits(opcode)) {
            return 19;
        }

        std::vector<std::uint8_t> bytes{
            static_cast<std::uint8_t>(opcode),
            static_cast<std::uint8_t>(opcode >> 8),
        };
        // SetSelectEx has no arguments (ESC_NOT, ...)

        const auto instruction = th2::decode_instruction(bytes, 0);
        const std::array<std::int32_t, 50> registers{};
        const auto event =
            th2::decode_event(instruction, bytes, registers);

        if (event.instruction.name != "SetSelectEx") {
            return 20;
        }
        if (event.arguments.size() != 0) {
            return 21;
        }
    }

    // --- Test 5: ScriptRuntime yields choice events ---
    {
        // Build a minimal scenario: SetSelectMes -> SetSelect -> End
        std::vector<std::uint8_t> bytes(scenario_header_size);
        bytes[0] = 'L';
        bytes[2] = 'F';
        write_u32(bytes, 8, 1);  // block 0 offset = 1 scenario block

        const auto set_select_mes_opcode = find_opcode("SetSelectMes");
        const auto set_select_opcode = find_opcode("SetSelect");

        // SetSelectMes: opcode (2 bytes) + STRING8 + NUM + NUM + NUM
        bytes.push_back(static_cast<std::uint8_t>(set_select_mes_opcode));
        bytes.push_back(
            static_cast<std::uint8_t>(set_select_mes_opcode >> 8));
        const std::string mes = "Choice 1";
        bytes.push_back(static_cast<std::uint8_t>(mes.size()));
        bytes.insert(bytes.end(), mes.begin(), mes.end());
        bytes.push_back(1);  // mode=immediate
        append_u32(bytes, static_cast<std::uint32_t>(-1));  // flag_no
        bytes.push_back(1);  // mode=immediate
        append_u32(bytes, 0);  // flag_value
        bytes.push_back(1);  // mode=immediate
        append_u32(bytes, 0);  // mes_wait

        // SetSelect: opcode (2 bytes) + REG
        bytes.push_back(static_cast<std::uint8_t>(set_select_opcode));
        bytes.push_back(
            static_cast<std::uint8_t>(set_select_opcode >> 8));
        bytes.push_back(3);  // register index 3

        // End instruction
        bytes.insert(bytes.end(), {1, 0});

        write_u32(bytes, 4, bytes.size());

        const th2::Scenario scenario(bytes);
        th2::Vm vm(scenario);

        // First yield should be the SetSelectMes event
        auto result = vm.run();
        if (result.reason != th2::VmYield::event) {
            return 22;
        }
        if (result.instruction.name != "SetSelectMes") {
            return 23;
        }

        // Second yield should be the SetSelect event
        result = vm.run();
        if (result.reason != th2::VmYield::event) {
            return 24;
        }
        if (result.instruction.name != "SetSelect") {
            return 25;
        }

        // Third yield should be the End
        result = vm.run();
        if (result.reason != th2::VmYield::ended) {
            return 26;
        }
    }

    return 0;
}
