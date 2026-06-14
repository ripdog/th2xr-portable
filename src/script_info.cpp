#include "archive.hpp"
#include "script_runtime.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: th2-script-info SDT.PAK START.SDT\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        th2::ScriptRuntime runtime(archive);
        runtime.load(argv[2]);
        for (std::size_t count = 0; count < 10000; ++count) {
            const auto step = runtime.run();
            std::cout << step.script_name << ": ";
            if (step.reason == th2::VmYield::event) {
                std::cout << step.event.instruction.name;
                for (const auto& argument : step.event.arguments) {
                    if (const auto* text = std::get_if<std::string>(&argument)) {
                        std::cout << " \"" << *text << '"';
                    } else if (const auto* number = std::get_if<std::int32_t>(&argument)) {
                        std::cout << ' ' << *number;
                    }
                }
            } else {
                std::cout << step.event.instruction.name;
            }
            std::cout << '\n';
            if (step.reason == th2::VmYield::ended) {
                return 0;
            }
        }
        throw std::runtime_error("script chain exceeded step limit");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
