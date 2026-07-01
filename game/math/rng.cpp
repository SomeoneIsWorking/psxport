#include "rng.h"
#include "core.h"

int32_t Rng::next() {
  uint32_t s = core->mem_r32(SEED_ADDR) * 0x41C64E6Du + 0x3039u;
  core->mem_w32(SEED_ADDR, s);
  return (int32_t)((s >> 16) & 0x7FFFu);
}
