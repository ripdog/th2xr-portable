#include "script_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace th2 {
namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t position)
{
    return static_cast<std::uint16_t>(bytes[position])
        | (static_cast<std::uint16_t>(bytes[position + 1]) << 8);
}

bool ends_with_sdt(const std::string& name)
{
    if (name.size() < 4) {
        return false;
    }
    std::string extension = name.substr(name.size() - 4);
    std::ranges::transform(extension, extension.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return extension == ".sdt";
}

std::int32_t integer(const Event& event, std::size_t index)
{
    return std::get<std::int32_t>(event.arguments.at(index));
}

RegisterTarget target(const Event& event, std::size_t index)
{
    return std::get<RegisterTarget>(event.arguments.at(index));
}

std::int32_t loop(std::int32_t value, std::int32_t limit)
{
    const auto result = value % limit;
    return result < 0 ? result + limit : result;
}

}  // namespace

ScriptRuntime::ScriptRuntime(const Archive& archive)
    : archive_(&archive)
{
}

void ScriptRuntime::load(std::string name)
{
    if (!ends_with_sdt(name)) {
        name += ".sdt";
    }
    const auto* entry = archive_->find(name);
    if (!entry) {
        throw std::runtime_error("scenario not found: " + name);
    }
    scenario_ = std::make_unique<Scenario>(archive_->read(*entry));
    vm_ = std::make_unique<Vm>(*scenario_);
    script_name_ = std::move(name);
}

std::int32_t ScriptRuntime::flag(std::size_t index) const
{
    return flags_.at(index);
}

std::int32_t ScriptRuntime::game_flag(std::size_t index) const
{
    return game_flags_.at(index);
}

void ScriptRuntime::set_flag(std::size_t index, std::int32_t value)
{
    flags_.at(index) = value;
}

void ScriptRuntime::set_game_flag(std::size_t index, std::int32_t value)
{
    game_flags_.at(index) = value;
}

void ScriptRuntime::set_reg(std::size_t index, std::int32_t value)
{
    vm_->set_reg(index, value);
}

std::int32_t ScriptRuntime::reg(std::size_t index) const
{
    return vm_->reg(index);
}

std::span<const std::int32_t> ScriptRuntime::vm_registers() const
{
    return vm_->registers();
}

std::span<const std::int32_t> ScriptRuntime::vm_stack() const
{
    return vm_->stack();
}

std::size_t ScriptRuntime::vm_pc() const
{
    return vm_->pc();
}

void ScriptRuntime::vm_restore(
    std::span<const std::int32_t> registers,
    std::span<const std::int32_t> stack,
    std::size_t pc)
{
    vm_->set_pc(pc);
    vm_->reset_stack();
    for (const auto value : stack) {
        vm_->push_stack(value);
    }
    for (std::size_t i = 0; i < registers.size() && i < Vm::register_count; ++i) {
        vm_->set_reg(i, registers[i]);
    }
}

bool ScriptRuntime::handle(const Event& event)
{
    if (event.instruction.name == "SetFlag") {
        const auto index = integer(event, 0);
        const auto value = integer(event, 1);
        if (index >= 50 && index < 100) {
            game_flags_.at(index) = value;
            static constexpr std::array completion_flags{
                81, 83, 84, 85, 86, 88, 89, 90, 93,
            };
            game_flags_[80] = static_cast<std::int32_t>(std::ranges::count_if(
                completion_flags, [this](int flag) {
                    return game_flags_[flag] != 0;
                }));
        } else {
            flags_.at(index) = value;
        }
        return true;
    }
    if (event.instruction.name == "GetFlag") {
        const auto index = integer(event, 0);
        const auto output = target(event, 1);
        vm_->set_reg(output.index,
            (index >= 50 && index < 100 ? game_flags_ : flags_).at(index));
        return true;
    }
    if (event.instruction.name == "SetGameFlag") {
        game_flags_.at(integer(event, 0)) = integer(event, 1);
        return true;
    }
    if (event.instruction.name == "GetGameFlag") {
        vm_->set_reg(target(event, 1).index, game_flags_.at(integer(event, 0)));
        return true;
    }
    if (event.instruction.name == "LoadScriptNum") {
        auto name = std::to_string(integer(event, 0));
        if (name.size() < 5) {
            name.insert(name.begin(), 5 - name.size(), '0');
        }
        load(name);
        return true;
    }
    if (event.instruction.name == "Mov2") {
        vm_->set_reg(target(event, 0).index, integer(event, 1));
        return true;
    }
    if (event.instruction.name == "Sin"
        || event.instruction.name == "Cos") {
        constexpr double pi = 3.14159265358979323846;
        const auto angle = loop(integer(event, 1), 3600) * pi / 1800.0;
        const auto scale = integer(event, 2) < 0 ? 4096 : integer(event, 2);
        const auto value = event.instruction.name == "Sin"
            ? std::sin(angle) : std::cos(angle);
        vm_->set_reg(
            target(event, 0).index,
            static_cast<std::int32_t>(value * scale));
        return true;
    }
    if (event.instruction.name == "Abs") {
        vm_->set_reg(target(event, 0).index, std::abs(integer(event, 1)));
        return true;
    }
    if (event.instruction.name == "GetTime") {
        const auto milliseconds = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        vm_->set_reg(
            target(event, 0).index,
            static_cast<std::int32_t>(
                static_cast<std::uint32_t>(milliseconds)));
        return true;
    }
    if (event.instruction.name == "GetSystemTime") {
        const auto now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        vm_->set_reg(target(event, 0).index, local.tm_hour);
        vm_->set_reg(target(event, 1).index, local.tm_mday);
        vm_->set_reg(target(event, 2).index, local.tm_mon + 1);
        vm_->set_reg(target(event, 3).index, local.tm_year + 1900);
        return true;
    }
    if (event.instruction.name == "VIB") {
        return true;
    }
    return false;
}

ScriptStep ScriptRuntime::run()
{
    if (!vm_) {
        throw std::runtime_error("no scenario is loaded");
    }
    while (true) {
        const auto result = vm_->run();
        if (result.reason == VmYield::event) {
            auto event = decode_event(result.instruction, result.bytes, vm_->registers());
            if (handle(event)) {
                continue;
            }
            return {result.reason, std::move(event), script_name_, 0};
        }
        if (result.reason == VmYield::load_script) {
            const auto length = result.bytes[2];
            load(std::string(
                reinterpret_cast<const char*>(result.bytes.data() + 3), length));
            continue;
        }
        const std::uint32_t wait_value =
            (result.reason == VmYield::wait_frames || result.reason == VmYield::wait_time)
            ? read_u16(result.bytes, 2) : 0u;
        return {result.reason, {}, script_name_, wait_value};
    }
}

}  // namespace th2
