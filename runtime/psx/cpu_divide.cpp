#include "cpu_divide.h"

#include "core.h"

#include <cstdint>

void cpu_div(Core *core, uint32_t numerator, uint32_t denominator) {
  const int32_t signedNumerator = static_cast<int32_t>(numerator);
  const int32_t signedDenominator = static_cast<int32_t>(denominator);
  if (signedDenominator == 0) {
    core->lo = signedNumerator < 0 ? 1u : 0xFFFFFFFFu;
    core->hi = static_cast<uint32_t>(signedNumerator);
  } else if (numerator == 0x80000000u && signedDenominator == -1) {
    core->lo = 0x80000000u;
    core->hi = 0;
  } else {
    core->lo = static_cast<uint32_t>(signedNumerator / signedDenominator);
    core->hi = static_cast<uint32_t>(signedNumerator % signedDenominator);
  }
}

void cpu_divu(Core *core, uint32_t numerator, uint32_t denominator) {
  if (denominator == 0) {
    core->lo = 0xFFFFFFFFu;
    core->hi = numerator;
  } else {
    core->lo = numerator / denominator;
    core->hi = numerator % denominator;
  }
}
