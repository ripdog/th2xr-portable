#include "archive.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

bool has_sdt_extension(std::string_view name)
{
    if (name.size() < 4) {
        return false;
    }
    const auto extension = name.substr(name.size() - 4);
    return extension == ".SDT" || extension == ".sdt";
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: th2-scenario-info SDT.PAK\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        std::size_t scenario_count = 0;
        std::size_t block_count = 0;
        std::size_t bytecode_size = 0;

        for (const auto& entry : archive.entries()) {
            if (!has_sdt_extension(entry.name)) {
                continue;
            }
            try {
                const th2::Scenario scenario(archive.read(entry));
                ++scenario_count;
                bytecode_size += scenario.bytecode().size();
                block_count += std::count_if(
                    scenario.blocks().begin(), scenario.blocks().end(),
                    [](std::uint32_t address) { return address != 0; });
            } catch (const std::exception& error) {
                throw std::runtime_error(entry.name + ": " + error.what());
            }
        }

        std::cout << std::filesystem::path(argv[1]) << ": "
                  << scenario_count << " valid scenarios, "
                  << block_count << " blocks, "
                  << bytecode_size << " bytecode bytes\n";
        return scenario_count == 0 ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
