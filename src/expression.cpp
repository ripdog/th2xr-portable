#include "expression.hpp"

#include <cctype>
#include <stdexcept>

namespace th2 {
namespace {

class Parser {
public:
    Parser(std::string_view expression, std::span<const std::int32_t> registers)
        : expression_(expression), registers_(registers)
    {
    }

    std::int32_t parse()
    {
        const auto result = sum();
        if (position_ != expression_.size()) {
            throw std::runtime_error("unexpected character in expression");
        }
        return result;
    }

private:
    std::string_view expression_;
    std::span<const std::int32_t> registers_;
    std::size_t position_ = 0;

    std::int32_t sum()
    {
        auto result = product();
        while (position_ < expression_.size()
               && (expression_[position_] == '+' || expression_[position_] == '-')) {
            const auto operation = expression_[position_++];
            const auto right = product();
            result = operation == '+' ? result + right : result - right;
        }
        return result;
    }

    std::int32_t product()
    {
        auto result = factor();
        while (position_ < expression_.size()
               && (expression_[position_] == '*' || expression_[position_] == '/'
                   || expression_[position_] == '%')) {
            const auto operation = expression_[position_++];
            const auto right = factor();
            if ((operation == '/' || operation == '%') && right == 0) {
                throw std::runtime_error("division by zero in expression");
            }
            if (operation == '*') {
                result *= right;
            } else if (operation == '/') {
                result /= right;
            } else {
                result %= right;
            }
        }
        return result;
    }

    std::int32_t factor()
    {
        bool negative = false;
        while (position_ < expression_.size() && expression_[position_] == '-') {
            negative = !negative;
            ++position_;
        }

        std::int32_t result = 0;
        if (position_ < expression_.size() && expression_[position_] == '[') {
            ++position_;
            result = sum();
            if (position_ >= expression_.size() || expression_[position_] != ']') {
                throw std::runtime_error("unterminated expression group");
            }
            ++position_;
        } else if (expression_.substr(position_, 3) == "reg") {
            position_ += 3;
            const auto index = unsigned_integer(10);
            if (index >= registers_.size()) {
                throw std::runtime_error("expression register index is out of range");
            }
            result = registers_[index];
        } else {
            int base = 10;
            if (expression_.substr(position_, 2) == "0x") {
                position_ += 2;
                base = 16;
            }
            result = static_cast<std::int32_t>(unsigned_integer(base));
        }
        return negative ? -result : result;
    }

    std::uint32_t unsigned_integer(int base)
    {
        const auto start = position_;
        std::uint32_t result = 0;
        while (position_ < expression_.size()) {
            const auto byte = static_cast<unsigned char>(expression_[position_]);
            int digit = -1;
            if (std::isdigit(byte)) {
                digit = byte - '0';
            } else if (base == 16 && std::isxdigit(byte)) {
                digit = std::tolower(byte) - 'a' + 10;
            }
            if (digit < 0 || digit >= base) {
                break;
            }
            result = result * base + static_cast<std::uint32_t>(digit);
            ++position_;
        }
        if (position_ == start) {
            throw std::runtime_error("expected integer in expression");
        }
        return result;
    }
};

}  // namespace

std::int32_t evaluate_expression(
    std::string_view expression, std::span<const std::int32_t> registers)
{
    return Parser(expression, registers).parse();
}

}  // namespace th2
