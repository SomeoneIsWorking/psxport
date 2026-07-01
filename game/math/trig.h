// class Trig — PC-native libgte trig helpers (rsin, rcos, ratan2, isqrt).
//
// PROPER OOP: stateless static methods (a helper namespace as a class). Same pattern as Mtx —
// each method corresponds ONE-TO-ONE to a resident libgte leaf.
//
// The angle unit is the PSX 12-bit angle where 4096 == 2π; results are Q12 fixed (in [-4096, 4096]
// for sin/cos). The sine table lives at guest 0x800A5AF0 in MAIN.EXE .rodata (1024 int16 entries
// for sin(0..π/2)). Native methods read that table via the Core so state stays consistent with any
// still-substrate reader.
#pragma once
#include <cstdint>
class Core;

class Trig {
public:
  static constexpr uint32_t SIN_TAB = 0x800A5AF0u;

  // rsin (guest FUN_80083E80): sin(angle12) -> Q12 in [-4096, 4096].
  static int32_t rsin(Core* c, int32_t angle);
  // rcos (guest FUN_80083F50): cos(angle12) -> Q12.
  static int32_t rcos(Core* c, int32_t angle);
};
