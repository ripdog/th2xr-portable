#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace th2 {

std::int32_t evaluate_expression(
    std::string_view expression, std::span<const std::int32_t> registers);

}  // namespace th2
