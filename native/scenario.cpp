#include "scenario.hpp"

#include <algorithm>
#include <stdexcept>

namespace th2 {
namespace {

constexpr std::size_t header_size = 2 * sizeof(std::uint16_t)
    + sizeof(std::uint32_t)
    + Scenario::block_count * sizeof(std::uint32_t);

std::uint16_t read_u16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t read_u32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

}  // namespace

Scenario::Scenario(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < header_size) {
        throw std::runtime_error("truncated scenario header");
    }
    if (read_u16(bytes.data()) != 'L' || read_u16(bytes.data() + 2) != 'F') {
        throw std::runtime_error("invalid scenario signature");
    }

    const std::size_t declared_size = read_u32(bytes.data() + 4);
    if (declared_size != bytes.size()) {
        throw std::runtime_error("scenario file size does not match header");
    }

    for (std::size_t index = 0; index < blocks_.size(); ++index) {
        blocks_[index] = read_u32(bytes.data() + 8 + index * sizeof(std::uint32_t));
    }
    bytecode_.assign(bytes.begin() + header_size, bytes.end());

    for (const auto address : blocks_) {
        if (address > bytecode_.size()) {
            throw std::runtime_error("scenario block address is outside bytecode");
        }
    }
}

std::span<const std::uint8_t> Scenario::block(std::size_t index) const
{
    if (index >= blocks_.size()) {
        throw std::out_of_range("scenario block index is out of range");
    }
    const auto address = blocks_[index];
    if (address == 0) {
        return {};
    }

    std::size_t end = bytecode_.size();
    for (const auto candidate : blocks_) {
        if (candidate > address) {
            end = std::min(end, static_cast<std::size_t>(candidate - 1));
        }
    }
    return std::span<const std::uint8_t>(bytecode_).subspan(address - 1, end - address + 1);
}

}  // namespace th2
