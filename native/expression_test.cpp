#include "expression.hpp"

#include <array>
#include <cstdint>

int main()
{
    std::array<std::int32_t, 50> registers{};
    registers[3] = 7;
    if (th2::evaluate_expression("2+3*4", registers) != 14
        || th2::evaluate_expression("[2+3]*4", registers) != 20
        || th2::evaluate_expression("reg3*2-0x3", registers) != 11
        || th2::evaluate_expression("--5", registers) != 5) {
        return 1;
    }
}
