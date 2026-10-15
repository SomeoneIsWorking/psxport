// R3000 DIV/DIVU result semantics shared by emitted and interpreted guest code.
#pragma once

#include <stdint.h>

class Core;

namespace psx::cpu {

// The operands of a DIV/DIVU as the registers hold them.
struct Division {
  uint32_t numerator;
  uint32_t denominator;
};

// The R3000 does not trap on division by zero or signed overflow; these write the architecturally defined
// quotient/remainder pair directly to Core::lo/Core::hi.
void divide(Core *core, Division operands);
void divideUnsigned(Core *core, Division operands);

} // namespace psx::cpu
