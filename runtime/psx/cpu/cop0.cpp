#include "core.h"
#include "game.h"

#include <cstdint>

// COP0 register 12 is Status; bit zero is the CPU master interrupt enable. Keep the registers
// per-Core so exception state cannot leak between runtime instances.
std::uint32_t cop0_mfc(Core *core, std::uint32_t reg) {
  return reg < 16 ? core->cop0[reg] : 0;
}

void cop0_mtc(Core *core, std::uint32_t reg, std::uint32_t value) {
  if (reg >= 16) {
    return;
  }
  core->cop0[reg] = value;
  if (reg == 12) {
    core->game->hle.irq_enabled = (value & 1u) ? 1 : 0;
  }
}
