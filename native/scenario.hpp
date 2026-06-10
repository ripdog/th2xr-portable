#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace th2 {

class Scenario {
public:
    static constexpr std::size_t block_count = 256;

    explicit Scenario(std::span<const std::uint8_t> bytes);

    const std::array<std::uint32_t, block_count>& blocks() const { return blocks_; }
    std::span<const std::uint8_t> bytecode() const { return bytecode_; }
    std::span<const std::uint8_t> block(std::size_t index) const;

private:
    std::array<std::uint32_t, block_count> blocks_{};
    std::vector<std::uint8_t> bytecode_;
};

}  // namespace th2
