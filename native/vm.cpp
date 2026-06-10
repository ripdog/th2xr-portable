#include "vm.hpp"

#include <algorithm>
#include <stdexcept>

namespace th2 {
namespace {

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t position)
{
    return static_cast<std::uint32_t>(bytes[position])
        | (static_cast<std::uint32_t>(bytes[position + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[position + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[position + 3]) << 24);
}

std::int32_t arithmetic(std::uint16_t opcode, std::int32_t left, std::int32_t right)
{
    switch ((opcode - 16) / 2) {
    case 0: return left + right;
    case 1: return left - right;
    case 2: return left * right;
    case 3: return right == 0 ? 0 : left / right;
    case 4: return right == 0 ? 0 : left % right;
    case 5: return left & right;
    case 6: return left | right;
    case 7: return left ^ right;
    default: throw std::runtime_error("invalid arithmetic opcode");
    }
}

}  // namespace

Vm::Vm(const Scenario& scenario, std::size_t block)
    : scenario_(&scenario)
{
    if (block >= scenario.blocks().size() || scenario.blocks()[block] == 0) {
        throw std::runtime_error("scenario block is not present");
    }
    pc_ = scenario.blocks()[block] - 1;
}

std::int32_t Vm::reg(std::size_t index) const
{
    if (index >= registers_.size()) {
        throw std::out_of_range("register index is out of range");
    }
    return registers_[index];
}

void Vm::set_reg(std::size_t index, std::int32_t value)
{
    if (index >= registers_.size()) {
        throw std::out_of_range("register index is out of range");
    }
    registers_[index] = value;
}

std::int32_t Vm::value(std::size_t position, bool immediate) const
{
    const auto code = scenario_->bytecode();
    if (immediate) {
        return static_cast<std::int32_t>(read_u32(code, position));
    }
    return reg(code[position]);
}

bool Vm::compare(std::int32_t left, std::uint8_t operation, std::int32_t right) const
{
    switch (operation) {
    case 0: return left < right;
    case 1: return left <= right;
    case 2: return left > right;
    case 3: return left >= right;
    case 4: return left == right;
    case 5: return left != right;
    default: throw std::runtime_error("invalid comparison operation");
    }
}

void Vm::jump(std::uint32_t address)
{
    if (address >= scenario_->bytecode().size()) {
        throw std::runtime_error("bytecode jump is out of range");
    }
    pc_ = address;
}

void Vm::execute(const Instruction& instruction)
{
    const auto code = scenario_->bytecode();
    const auto pc = pc_;
    const auto opcode = instruction.opcode;

    switch (opcode) {
    case 0:
        pc_ += instruction.size;
        break;
    case 1:
        ended_ = true;
        break;
    case 2:
    case 3:
        set_reg(code[pc + 2], value(pc + 3, opcode == 3));
        pc_ += instruction.size;
        break;
    case 4:
        std::swap(registers_.at(code[pc + 2]), registers_.at(code[pc + 3]));
        pc_ += instruction.size;
        break;
    case 5:
        registers_.at(code[pc + 2]) =
            static_cast<std::int32_t>((pc_ * 1103515245u + 12345u) & 0xffff);
        pc_ += instruction.size;
        break;
    case 6:
    case 7: {
        const bool immediate = opcode == 7;
        const auto right = value(pc + 4, immediate);
        const auto target_position = pc + (immediate ? 8 : 5);
        if (compare(reg(code[pc + 2]), code[pc + 3], right)) {
            jump(read_u32(code, target_position));
        } else {
            pc_ += instruction.size;
        }
        break;
    }
    case 8:
    case 9: {
        const bool immediate = opcode == 9;
        const auto right = value(pc + 4, immediate);
        const auto first_target = pc + (immediate ? 8 : 5);
        const auto target = compare(reg(code[pc + 2]), code[pc + 3], right)
            ? first_target : first_target + 4;
        jump(read_u32(code, target));
        break;
    }
    case 10:
        if (reg(code[pc + 2]) > 0) {
            --registers_.at(code[pc + 2]);
            jump(read_u32(code, pc + 3));
        } else {
            pc_ += instruction.size;
        }
        break;
    case 11:
        jump(read_u32(code, pc + 2));
        break;
    case 12:
        ++registers_.at(code[pc + 2]);
        pc_ += instruction.size;
        break;
    case 13:
        --registers_.at(code[pc + 2]);
        pc_ += instruction.size;
        break;
    case 14:
        registers_.at(code[pc + 2]) = ~reg(code[pc + 2]);
        pc_ += instruction.size;
        break;
    case 15:
        registers_.at(code[pc + 2]) = -reg(code[pc + 2]);
        pc_ += instruction.size;
        break;
    default:
        if (opcode >= 16 && opcode <= 31) {
            const auto destination = code[pc + 2];
            const auto right = value(pc + 3, (opcode & 1) != 0);
            registers_.at(destination) = arithmetic(opcode, reg(destination), right);
            pc_ += instruction.size;
        } else if (opcode == 32) {
            struct Factor {
                std::uint8_t type;
                std::int32_t data;
            };
            std::vector<Factor> factors;
            const auto expression_size = instruction.size - 5;
            std::size_t position = pc + 5;
            const auto end = position + expression_size;
            while (position < end) {
                const auto type = code[position++];
                if (type == 0) {
                    factors.push_back({type, static_cast<std::int32_t>(
                        read_u32(code, position))});
                    position += 4;
                } else if (type == 1 || type == 2) {
                    factors.push_back({type, code[position++]});
                } else {
                    throw std::runtime_error("invalid calculation factor");
                }
            }
            while (factors.size() > 1) {
                const auto operation = std::find_if(
                    factors.begin(), factors.end(),
                    [](const Factor& factor) { return factor.type == 2; });
                if (operation < factors.begin() + 2 || operation == factors.end()) {
                    throw std::runtime_error("invalid calculation expression");
                }
                const auto left_factor = *(operation - 2);
                const auto right_factor = *(operation - 1);
                const auto left = left_factor.type == 0
                    ? left_factor.data : reg(left_factor.data);
                const auto right = right_factor.type == 0
                    ? right_factor.data : reg(right_factor.data);
                const auto result = arithmetic(
                    16 + static_cast<std::uint16_t>(operation->data) * 2,
                    left, right);
                const auto index = static_cast<std::size_t>(
                    std::distance(factors.begin(), operation - 2));
                factors.erase(operation - 2, operation + 1);
                factors.insert(factors.begin() + index, {0, result});
            }
            if (factors.empty()) {
                throw std::runtime_error("empty calculation expression");
            }
            set_reg(code[pc + 2], factors.front().type == 0
                ? factors.front().data : reg(factors.front().data));
            pc_ += instruction.size;
        } else if (opcode == 33) {
            stack_.insert(stack_.end(), registers_.begin(), registers_.end());
            pc_ += instruction.size;
        } else if (opcode == 34) {
            if (stack_.size() < registers_.size()) {
                throw std::runtime_error("VM stack underflow");
            }
            std::copy(stack_.end() - registers_.size(), stack_.end(), registers_.begin());
            stack_.resize(stack_.size() - registers_.size());
            pc_ += instruction.size;
        } else if (opcode == 35) {
            stack_.push_back(static_cast<std::int32_t>(pc + instruction.size));
            const auto block = code[pc + 2];
            if (scenario_->blocks()[block] == 0) {
                throw std::runtime_error("call target block is not present");
            }
            pc_ = scenario_->blocks()[block] - 1;
        } else if (opcode == 36) {
            if (stack_.empty()) {
                throw std::runtime_error("VM stack underflow");
            }
            pc_ = static_cast<std::size_t>(stack_.back());
            stack_.pop_back();
        } else if (opcode == 41) {
            pc_ += instruction.size;
        } else {
            throw std::runtime_error("core opcode execution is not implemented");
        }
    }
}

VmResult Vm::run()
{
    if (ended_) {
        return {VmYield::ended, {}, {}};
    }

    while (true) {
        const auto instruction = decode_instruction(scenario_->bytecode(), pc_);
        if (instruction.opcode >= 64) {
            pc_ += instruction.size;
            return {VmYield::event, instruction,
                    scenario_->bytecode().subspan(instruction.offset, instruction.size)};
        }
        if (instruction.opcode == 1) {
            execute(instruction);
            return {VmYield::ended, instruction, {}};
        }
        if (instruction.opcode >= 37 && instruction.opcode <= 40) {
            pc_ += instruction.size;
            const auto reason = instruction.opcode == 37 ? VmYield::wait_frames
                : instruction.opcode == 38 ? VmYield::wait_time
                : instruction.opcode == 39 ? VmYield::frame
                : VmYield::load_script;
            return {reason, instruction,
                    scenario_->bytecode().subspan(instruction.offset, instruction.size)};
        }
        execute(instruction);
    }
}

}  // namespace th2
