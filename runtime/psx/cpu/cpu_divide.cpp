#include "cpu_divide.h"

#include "core.h"

namespace psx::cpu {

void divide(Core *core, Division operands) {
  core->lo = quotient(operands);
  core->hi = remainder(operands);
}

void divideUnsigned(Core *core, Division operands) {
  if (operands.denominator == 0) {
    core->lo = 0xFFFFFFFFu;
    core->hi = operands.numerator;
  } else {
    core->lo = operands.numerator / operands.denominator;
    core->hi = operands.numerator % operands.denominator;
  }
}

} // namespace psx::cpu
