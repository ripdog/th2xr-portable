#pragma once

#include "archive.hpp"
#include "event.hpp"
#include "scenario.hpp"
#include "vm.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace th2 {

struct ScriptStep {
    VmYield reason;
    Event event;
    std::string script_name;
    std::uint32_t wait_value = 0;
};

class ScriptRuntime {
public:
    explicit ScriptRuntime(const Archive& archive);

    void load(std::string name);
    void load_file(const std::filesystem::path& path);
    ScriptStep run();

    std::int32_t flag(std::size_t index) const;
    std::int32_t game_flag(std::size_t index) const;
    void set_flag(std::size_t index, std::int32_t value);
    void set_game_flag(std::size_t index, std::int32_t value);
    void reset_flags();
    void set_reg(std::size_t index, std::int32_t value);
    std::int32_t reg(std::size_t index) const;
    const std::string& script_name() const { return script_name_; }

    std::span<const std::int32_t> vm_registers() const;
    std::span<const std::int32_t> vm_stack() const;
    std::span<const std::uint8_t> vm_bytecode() const;
    std::size_t vm_pc() const;
    void vm_restore(std::span<const std::int32_t> registers,
                    std::span<const std::int32_t> stack,
                    std::size_t pc);
    std::span<const std::int32_t> all_flags() const { return flags_; }
    std::span<const std::int32_t> all_game_flags() const { return game_flags_; }

private:
    const Archive* archive_;
    std::unique_ptr<Scenario> scenario_;
    std::unique_ptr<Vm> vm_;
    std::array<std::int32_t, 1024> flags_{};
    std::array<std::int32_t, 1024> game_flags_{};
    std::string script_name_;
    std::filesystem::path loose_script_directory_;

    bool handle(const Event& event);
    void load_bytes(std::vector<std::uint8_t> bytes, std::string name);
};

}  // namespace th2
