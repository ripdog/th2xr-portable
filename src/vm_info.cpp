#include "archive.hpp"
#include "event.hpp"
#include "scenario.hpp"
#include "vm.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: th2-vm-info SDT.PAK SCENARIO.SDT [END_OFFSET]\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        const auto* entry = archive.find(argv[2]);
        if (!entry) {
            throw std::runtime_error("scenario not found");
        }
        const th2::Scenario scenario(archive.read(*entry));
        if (argc == 4) {
            const auto target = static_cast<std::size_t>(
                std::stoull(argv[3]));
            std::size_t offset = 0;
            while (offset < scenario.bytecode().size()) {
                const auto instruction =
                    th2::decode_instruction(scenario.bytecode(), offset);
                if (offset + instruction.size >= target - std::min(target, std::size_t{100})
                    && offset <= target + 300) {
                    std::cout << instruction.offset << ".."
                              << instruction.offset + instruction.size << ": "
                              << instruction.name;
                    if (instruction.opcode >= 64) {
                        const auto event = th2::decode_event(
                            instruction,
                            scenario.bytecode().subspan(
                                instruction.offset, instruction.size),
                            {});
                        for (const auto& argument : event.arguments) {
                            if (const auto* text =
                                    std::get_if<std::string>(&argument)) {
                                std::cout << " text[" << text->size() << "]=\"";
                                for (const unsigned char byte : *text) {
                                    if (byte == '\\') std::cout << "\\\\";
                                    else if (byte == '\n') std::cout << "\\n";
                                    else if (byte == '\r') std::cout << "\\r";
                                    else if (byte == '\t') std::cout << "\\t";
                                    else if (byte < 0x20 || byte >= 0x7f) {
                                        std::cout << "\\x" << std::hex
                                                  << static_cast<int>(byte)
                                                  << std::dec;
                                    } else {
                                        std::cout << byte;
                                    }
                                }
                                std::cout << '"';
                            } else if (const auto* number =
                                           std::get_if<std::int32_t>(&argument)) {
                                std::cout << ' ' << *number;
                            }
                        }
                    }
                    std::cout << '\n';
                }
                if (offset > target + 300) break;
                offset += instruction.size;
            }
            return 0;
        }
        th2::Vm vm(scenario);

        for (std::size_t count = 0; count < 10000; ++count) {
            const auto result = vm.run();
            std::cout << result.instruction.offset << ": "
                      << result.instruction.name;
            if (result.reason == th2::VmYield::event) {
                const auto event = th2::decode_event(
                    result.instruction, result.bytes, vm.registers());
                for (const auto& argument : event.arguments) {
                    if (const auto* text = std::get_if<std::string>(&argument)) {
                        std::cout << " \"" << *text << '"';
                    } else if (const auto* number = std::get_if<std::int32_t>(&argument)) {
                        std::cout << ' ' << *number;
                    }
                }
            }
            std::cout << '\n';
            if (result.reason == th2::VmYield::ended
                || result.reason == th2::VmYield::load_script) {
                return 0;
            }
        }
        throw std::runtime_error("scenario did not terminate or load another script");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
