// R3000 DIV/DIVU result semantics shared by emitted and interpreted guest code.
#pragma once

#include <stdint.h>

class Core;

extern "C" {

// The R3000 does not trap on division by zero or signed overflow. These entry points write the
// architecturally defined quotient/remainder pair directly to Core::lo/Core::hi.
void cpu_div(Core *core, uint32_t numerator, uint32_t denominator);
void cpu_divu(Core *core, uint32_t numerator, uint32_t denominator);
}
