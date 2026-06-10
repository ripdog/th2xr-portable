#pragma once

#include "bytecode.hpp"
#include "scenario.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace th2 {

enum class VmYield {
    event,
    frame,
    wait_frames,
    wait_time,
    load_script,
    ended,
};

struct VmResult {
    VmYield reason;
    Instruction instruction;
    std::span<const std::uint8_t> bytes;
};

class Vm {
public:
    static constexpr std::size_t register_count = 50;

    explicit Vm(const Scenario& scenario, std::size_t block = 0);

    VmResult run();
    void resume() {}

    std::int32_t reg(std::size_t index) const;
    void set_reg(std::size_t index, std::int32_t value);
    std::size_t pc() const { return pc_; }

private:
    const Scenario* scenario_;
    std::array<std::int32_t, register_count> registers_{};
    std::vector<std::int32_t> stack_;
    std::size_t pc_ = 0;
    bool ended_ = false;

    std::int32_t value(std::size_t position, bool immediate) const;
    bool compare(std::int32_t left, std::uint8_t operation, std::int32_t right) const;
    void jump(std::uint32_t address);
    void execute(const Instruction& instruction);
};

}  // namespace th2
