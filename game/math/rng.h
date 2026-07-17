// class Rng — the PC-native LFSR pseudo-random number generator (guest FUN_8009A450).
//
// PROPER OOP: one instance per Core (embedded as `Core::rng`), back-pointer to Core wired in
// Core::Core(). Callers use it as `rngOf(c).next()`. No `extern "C"` shim.
//
// The seed lives in guest memory at 0x80105EE8 (MAIN.EXE .bss, same location the substrate uses)
// so the RNG stream is SHARED with any still-substrate caller — no divergence between the two
// sides. This is deliberate: 0x8009A450 is called from many substrate leaves, and the seed's
// address is a hard ABI point (multiple retained fns literally lw/sw against 0x80105EE8).
//
// Algorithm: classic PSX libc rand() — LCG constants 0x41C64E6D, 12345 (0x3039).
#pragma once
#include <cstdint>
class Core;

class Rng {
public:
  static constexpr uint32_t SEED_ADDR = 0x80105EE8u;
  Core* core = nullptr;

  // Advance the LFSR and return the classic PSX rand() value in [0, 0x7FFF].
  int32_t next();

  // inRange(lo, hi): FUN_80032A44 — signed random integer in [lo, hi). Uses next() then scales
  //   ((next * (hi - lo)) >> 15) + lo — the ~15-bit space fills [0, hi - lo). Matches recomp
  //   verbatim: PSX MULT/MFLO on 32-bit signed operands + `sra 15`. Body from disas 0x80032A44.
  int32_t inRange(int32_t lo, int32_t hi);
};
