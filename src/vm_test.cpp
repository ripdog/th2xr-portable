#include "scenario.hpp"
#include "vm.hpp"

#include <cstdint>
#include <vector>

namespace {

constexpr std::size_t header_size = 1032;

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t position, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[position++] = static_cast<std::uint8_t>(value >> shift);
    }
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    const auto position = bytes.size();
    bytes.resize(position + 4);
    write_u32(bytes, position, value);
}

}  // namespace

int main()
{
    std::vector<std::uint8_t> bytes(header_size);
    bytes[0] = 'L';
    bytes[2] = 'F';
    write_u32(bytes, 8, 1);

    bytes.insert(bytes.end(), {3, 0, 0});
    append_u32(bytes, 5);
    bytes.insert(bytes.end(), {17, 0, 0});
    append_u32(bytes, 7);
    bytes.insert(bytes.end(), {
        32, 0, 1, 9, 0,
        1, 0,
        0, 3, 0, 0, 0,
        2, 0,
    });
    bytes.insert(bytes.end(), {68, 0});
    bytes.insert(bytes.end(), {39, 0, 1, 0});
    write_u32(bytes, 4, bytes.size());

    const th2::Scenario scenario(bytes);
    th2::Vm vm(scenario);
    const auto result = vm.run();
    if (result.reason != th2::VmYield::event || result.instruction.name != "BD"
        || vm.reg(0) != 12 || vm.reg(1) != 15) {
        return 1;
    }
    if (vm.run().reason != th2::VmYield::frame
        || vm.run().reason != th2::VmYield::ended) {
        return 2;
    }
}
