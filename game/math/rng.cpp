#include "rng.h"
#include "core.h"

int32_t Rng::next() {
  uint32_t s = core->mem_r32(SEED_ADDR) * 0x41C64E6Du + 0x3039u;
  core->mem_w32(SEED_ADDR, s);
  return (int32_t)((s >> 16) & 0x7FFFu);
}

// FUN_80032A44 — scaled random. Disas 0x80032A44..0x80032A84 verbatim: `sra v0, 15` on the 32×32
// MULT product. Both operands are treated as 32-bit signed (recomp: MULT after mov s0,a1/s1,a0 —
// no explicit sign-extension needed since a0/a1 arrive as 32-bit int registers).
int32_t Rng::inRange(int32_t lo, int32_t hi) {
  int32_t r = next();                                  // [0, 0x7FFF]
  int32_t span = hi - lo;                              // caller expected: hi >= lo (recomp doesn't check)
  int32_t prod = (int32_t)((int64_t)r * (int64_t)span);
  return (prod >> 15) + lo;
}
