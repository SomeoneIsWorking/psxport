// R3000 DIV/DIVU result semantics shared by emitted, interpreted and native guest code.
#pragma once

#include <cstdint>

class Core;

namespace psx::cpu {

// The operands of a DIV/DIVU as the registers hold them.
struct Division {
  std::uint32_t numerator;
  std::uint32_t denominator;
};

// DIV's LO and HI: no trap on a zero divisor or INT_MIN / -1, the architectural pair instead.
constexpr std::uint32_t quotient(Division operands) {
  const auto numerator = static_cast<std::int32_t>(operands.numerator);
  const auto denominator = static_cast<std::int32_t>(operands.denominator);
  if (denominator == 0) {
    return numerator < 0 ? 1u : 0xFFFFFFFFu;
  }
  if (operands.numerator == 0x80000000u && denominator == -1) {
    return 0x80000000u;
  }
  return static_cast<std::uint32_t>(numerator / denominator);
}
constexpr std::uint32_t remainder(Division operands) {
  const auto numerator = static_cast<std::int32_t>(operands.numerator);
  const auto denominator = static_cast<std::int32_t>(operands.denominator);
  if (denominator == 0) {
    return operands.numerator;
  }
  if (operands.numerator == 0x80000000u && denominator == -1) {
    return 0;
  }
  return static_cast<std::uint32_t>(numerator % denominator);
}

// DIV and DIVU into Core::lo / Core::hi.
void divide(Core *core, Division operands);
void divideUnsigned(Core *core, Division operands);

} // namespace psx::cpu
