#include "archive.hpp"
#include "scenario.hpp"
#include "vm.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: th2-vm-info SDT.PAK SCENARIO.SDT\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        const auto* entry = archive.find(argv[2]);
        if (!entry) {
            throw std::runtime_error("scenario not found");
        }
        const th2::Scenario scenario(archive.read(*entry));
        th2::Vm vm(scenario);

        for (std::size_t count = 0; count < 10000; ++count) {
            const auto result = vm.run();
            std::cout << result.instruction.offset << ": "
                      << result.instruction.name << '\n';
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
