#include "archive.hpp"
#include "bytecode.hpp"
#include "event.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::string_view, 87> retail_opcodes{
    "AddMessage2", "AddV", "B", "BC", "BCT", "BT", "C", "CL", "CP",
    "CR", "CRW", "CW", "Cos", "Dec", "End", "F", "FB", "GetFlag",
    "GetGameFlag", "GetSystemTime", "GetTime", "Goto", "H", "HT", "IfR",
    "IfV", "Inc", "K", "LoadScript", "Loop", "M", "MS", "MV", "MW",
    "Mov2", "MovV", "Q", "Rand", "ResetBmp", "Run", "S", "SE", "SEP",
    "SES", "SEV", "SEW", "SetBmpBright", "SetBmpEx", "SetBmpMove",
    "SetBmpParam", "SetBmpZoom", "SetDemoFlag", "SetEnding", "SetFlag",
    "SetGameFlag", "SetMapEvent", "SetMessage2", "SetMovie", "SetReplayNo",
    "SetSakura", "SetSelect", "SetSelectMes", "SetShake", "SetTimeMode",
    "SetTitle", "SetWeatherMode", "Sin", "SkipDate", "StopSakura", "T",
    "V", "VA", "VB", "VC", "VI", "VIB", "VS", "VT", "VV", "VW",
    "ViewCalender", "ViewClock", "W", "Wait", "WaitFrame", "WaitTime", "Z",
};

bool is_scenario(std::string_view name)
{
    if (name.size() < 4) {
        return false;
    }
    std::string extension(name.substr(name.size() - 4));
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension == ".sdt";
}

std::string scenario_name(std::string name)
{
    if (!is_scenario(name)) {
        name += ".SDT";
    }
    return name;
}

const std::string& string_argument(const th2::Event& event, std::size_t index)
{
    return std::get<std::string>(event.arguments.at(index));
}

struct Reference {
    std::string source;
    std::size_t offset;

    friend bool operator<(const Reference& left, const Reference& right)
    {
        return std::tie(left.source, left.offset)
            < std::tie(right.source, right.offset);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: th2-script-audit SDT.PAK\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        std::map<std::string, std::size_t> opcode_counts;
        std::map<std::string, std::set<Reference>> scenario_references;
        std::size_t scenario_count = 0;
        std::size_t instruction_count = 0;
        std::size_t undecodable_references = 0;
        std::array<std::int32_t, 50> registers{};
        registers.fill(1);

        for (const auto& entry : archive.entries()) {
            if (!is_scenario(entry.name)) {
                continue;
            }
            ++scenario_count;
            const th2::Scenario scenario(archive.read(entry));
            std::size_t offset = 0;
            try {
                while (offset < scenario.bytecode().size()) {
                    const auto instruction =
                        th2::decode_instruction(scenario.bytecode(), offset);
                    ++opcode_counts[std::string(instruction.name)];
                    ++instruction_count;

                    if (instruction.name == "SLoad") {
                        const auto bytes = scenario.bytecode().subspan(
                            instruction.offset, instruction.size);
                        const auto length = bytes[2];
                        const std::string target(
                            reinterpret_cast<const char*>(bytes.data() + 3),
                            length);
                        scenario_references[scenario_name(target)].insert(
                            {entry.name, instruction.offset});
                    } else if (instruction.name == "LoadScript"
                               || instruction.name == "SetMapEvent") {
                        try {
                            const auto event = th2::decode_event(
                                instruction,
                                scenario.bytecode().subspan(
                                    instruction.offset, instruction.size),
                                registers);
                            const auto argument =
                                instruction.name == "LoadScript" ? 0u : 3u;
                            const auto& target = string_argument(event, argument);
                            if (!target.empty()) {
                                scenario_references[scenario_name(target)].insert(
                                    {entry.name, instruction.offset});
                            }
                        } catch (const std::exception&) {
                            ++undecodable_references;
                        }
                    }
                    offset += instruction.size;
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    entry.name + " at byte " + std::to_string(offset)
                    + ": " + error.what());
            }
        }

        const std::set<std::string> expected(
            retail_opcodes.begin(), retail_opcodes.end());
        std::set<std::string> observed;
        for (const auto& [name, count] : opcode_counts) {
            static_cast<void>(count);
            observed.insert(name);
        }

        std::vector<std::string> unexpected;
        std::ranges::set_difference(
            observed, expected, std::back_inserter(unexpected));
        std::vector<std::string> absent;
        std::ranges::set_difference(
            expected, observed, std::back_inserter(absent));

        std::size_t present_references = 0;
        std::size_t missing_references = 0;
        for (const auto& [target, sources] : scenario_references) {
            if (archive.find(target)) {
                ++present_references;
            } else {
                ++missing_references;
                std::cout << "missing scenario " << target << " referenced by";
                for (const auto& source : sources) {
                    std::cout << ' ' << source.source << ':' << source.offset;
                }
                std::cout << '\n';
            }
        }

        std::cout << scenario_count << " scenarios, " << instruction_count
                  << " instructions, " << observed.size() << " opcodes\n"
                  << present_references << " present scenario destinations, "
                  << missing_references << " absent scenario destinations\n";
        if (undecodable_references != 0) {
            std::cout << undecodable_references
                      << " scenario references could not be decoded statically\n";
        }

        if (!unexpected.empty() || !absent.empty()) {
            for (const auto& name : unexpected) {
                std::cerr << "unexpected shipped opcode: " << name << '\n';
            }
            for (const auto& name : absent) {
                std::cerr << "expected shipped opcode is absent: " << name << '\n';
            }
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
