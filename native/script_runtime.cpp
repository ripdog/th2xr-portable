#include "script_runtime.hpp"

#include <algorithm>
#include <cctype>
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

void ScriptRuntime::set_reg(std::size_t index, std::int32_t value)
{
    vm_->set_reg(index, value);
}

std::int32_t ScriptRuntime::reg(std::size_t index) const
{
    return vm_->reg(index);
}

bool ScriptRuntime::handle(const Event& event)
{
    if (event.instruction.name == "SetFlag") {
        const auto index = integer(event, 0);
        const auto value = integer(event, 1);
        (index >= 50 && index < 100 ? game_flags_ : flags_).at(index) = value;
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
    if (event.instruction.name == "LoadScript") {
        load(std::get<std::string>(event.arguments.at(0)));
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
