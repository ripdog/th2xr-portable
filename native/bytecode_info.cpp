#include "archive.hpp"
#include "bytecode.hpp"
#include "event.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

bool is_scenario(std::string_view name)
{
    return name.size() >= 4
        && (name.substr(name.size() - 4) == ".SDT"
            || name.substr(name.size() - 4) == ".sdt");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: th2-bytecode-info SDT.PAK [OPCODE]\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        const std::string_view requested = argc == 3 ? argv[2] : "";
        std::map<std::string, std::size_t> counts;
        std::size_t instructions = 0;
        const std::array<std::int32_t, 50> registers{};

        for (const auto& entry : archive.entries()) {
            if (!is_scenario(entry.name)) {
                continue;
            }
            const th2::Scenario scenario(archive.read(entry));
            std::size_t offset = 0;
            try {
                while (offset < scenario.bytecode().size()) {
                    const auto instruction = th2::decode_instruction(
                        scenario.bytecode(), offset);
                    ++counts[std::string(instruction.name)];
                    ++instructions;
                    if (!requested.empty() && instruction.name == requested) {
                        const auto event = th2::decode_event(
                            instruction,
                            scenario.bytecode().subspan(
                                instruction.offset, instruction.size),
                            registers);
                        std::cout << entry.name << ':' << instruction.offset;
                        for (const auto& argument : event.arguments) {
                            std::visit([&](const auto& value) {
                                using T = std::decay_t<decltype(value)>;
                                if constexpr (std::is_same_v<T, std::int32_t>) {
                                    std::cout << ' ' << value;
                                } else if constexpr (
                                    std::is_same_v<T, std::string>) {
                                    std::cout << ' ' << std::quoted(value);
                                } else if constexpr (
                                    std::is_same_v<T, th2::RegisterTarget>) {
                                    std::cout << " reg[" << +value.index << ']';
                                } else {
                                    std::cout << " cmp["
                                              << +value.register_index << ','
                                              << +value.operation << ','
                                              << value.value << ']';
                                }
                            }, argument);
                        }
                        std::cout << '\n';
                    }
                    offset += instruction.size;
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    entry.name + " at byte " + std::to_string(offset)
                    + ": " + error.what());
            }
        }

        if (!requested.empty()) {
            return 0;
        }

        std::vector<std::pair<std::string, std::size_t>> sorted(
            counts.begin(), counts.end());
        std::ranges::sort(sorted, {}, &std::pair<std::string, std::size_t>::second);
        std::cout << instructions << " instructions, " << counts.size()
                  << " opcodes\n";
        for (std::size_t index = 0; index < sorted.size(); ++index) {
            const auto& [name, count] = sorted[sorted.size() - index - 1];
            std::cout << "  " << name << ": " << count << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
