#include "scenario.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t header_size = 1032;

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t position, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[position++] = static_cast<std::uint8_t>(value >> shift);
    }
}

bool rejects(const std::vector<std::uint8_t>& bytes)
{
    try {
        static_cast<void>(th2::Scenario(bytes));
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

}  // namespace

int main()
{
    std::vector<std::uint8_t> bytes(header_size + 4);
    bytes[0] = 'L';
    bytes[2] = 'F';
    write_u32(bytes, 4, bytes.size());
    write_u32(bytes, 8, 1);
    bytes[header_size] = 1;

    const th2::Scenario scenario(bytes);
    if (scenario.block(0).size() != 4 || !scenario.block(1).empty()) {
        return 1;
    }

    auto invalid = bytes;
    invalid[0] = 'X';
    if (!rejects(invalid)) {
        return 2;
    }

    invalid = bytes;
    write_u32(invalid, 4, bytes.size() + 1);
    if (!rejects(invalid)) {
        return 3;
    }

    invalid = bytes;
    write_u32(invalid, 8, 5);
    if (!rejects(invalid)) {
        return 4;
    }
}
