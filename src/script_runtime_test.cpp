#include "archive.hpp"
#include "script_runtime.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

void write_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::vector<std::uint8_t> make_kcap_archive(
    const std::string& entry_name,
    const std::vector<std::uint8_t>& entry_data)
{
    std::vector<std::uint8_t> file;
    file.insert(file.end(), {'K', 'C', 'A', 'P'});
    write_u32(file, 1);  // count

    std::array<char, 24> name{};
    const auto length = std::min(entry_name.size(), name.size());
    std::copy(entry_name.begin(), entry_name.begin() + length, name.begin());
    const std::uint32_t header_size =
        4 + 4 + (4 + 24 + 4 + 4);  // magic + count + one entry
    const std::uint32_t offset = header_size;

    write_u32(file, 0);  // type = uncompressed
    file.insert(file.end(), name.begin(), name.end());
    write_u32(file, offset);
    write_u32(file, static_cast<std::uint32_t>(entry_data.size()));
    file.insert(file.end(), entry_data.begin(), entry_data.end());
    return file;
}

std::vector<std::uint8_t> make_minimal_scenario(const std::string& script_name)
{
    // Header: 'L', 0, 'F', 0, size, 256 block addresses (all zero except block 0)
    std::vector<std::uint8_t> file(1032, 0);
    file[0] = 'L';
    file[2] = 'F';
    // Block 0 address = 1 (bytecode starts at offset 0)
    file[8] = 1;
    // Just an End instruction
    file.push_back(1);
    file.push_back(0);
    const auto size = static_cast<std::uint32_t>(file.size());
    for (int i = 0; i < 4; ++i) {
        file[4 + i] = static_cast<std::uint8_t>(size >> (i * 8));
    }
    static_cast<void>(script_name);
    return file;
}

}  // namespace

int main()
{
    const auto temp_dir = std::filesystem::temp_directory_path() / "th2_script_runtime_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto loose_path = temp_dir / "TEST.SDT";
    const auto archive_path = temp_dir / "SDT.PAK";

    {
        const auto loose_scenario = make_minimal_scenario("loose");
        std::ofstream loose(loose_path, std::ios::binary);
        loose.write(
            reinterpret_cast<const char*>(loose_scenario.data()),
            loose_scenario.size());
    }

    {
        const auto archive_scenario = make_minimal_scenario("archive");
        const auto archive = make_kcap_archive("test.sdt", archive_scenario);
        std::ofstream pak(archive_path, std::ios::binary);
        pak.write(reinterpret_cast<const char*>(archive.data()), archive.size());
    }

    th2::Archive scripts(archive_path);
    th2::ScriptRuntime runtime(scripts);

    // Load the loose file to set the loose-script directory.
    runtime.load_file(loose_path);
    if (runtime.script_name() != "TEST.SDT") {
        return 1;
    }

    // Now request the same script via the lowercase name used by LoadScript.
    // The loose file is uppercase .SDT; the engine must find it case-insensitively
    // instead of falling back to the archive's test.sdt entry.
    runtime.load("test");
    if (runtime.script_name() != "TEST.SDT") {
        return 2;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}
