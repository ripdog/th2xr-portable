#include "archive.hpp"
#include "bytecode.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

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
    if (argc != 2) {
        std::cerr << "usage: th2-bytecode-info SDT.PAK\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        std::map<std::string, std::size_t> counts;
        std::size_t instructions = 0;

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
                    offset += instruction.size;
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    entry.name + " at byte " + std::to_string(offset)
                    + ": " + error.what());
            }
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
