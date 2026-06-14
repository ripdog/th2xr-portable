#include "bytecode.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

int main()
{
    const std::vector<std::uint8_t> mov_value{
        3, 0, 4, 0x78, 0x56, 0x34, 0x12,
    };
    const auto mov = th2::decode_instruction(mov_value, 0);
    if (mov.name != "MovV" || mov.size != mov_value.size() || mov.waits) {
        return 1;
    }

    const std::vector<std::uint8_t> load{
        40, 0, 4, 't', 'e', 's', 't',
    };
    const auto sload = th2::decode_instruction(load, 0);
    if (sload.name != "SLoad" || sload.size != load.size() || !sload.waits) {
        return 2;
    }

    try {
        static_cast<void>(th2::decode_instruction({load.data(), 4}, 0));
        return 3;
    } catch (const std::runtime_error&) {
    }
}
