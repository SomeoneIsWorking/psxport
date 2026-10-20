// spu_device.cpp — the SPU's interrupt line into the bound Core's I_STAT.
#include "spu_device.h"
#include "core.h"
#include "game.h"
#include "irq_edge.h"

void psx::audio::spu::raiseIrq(Core &core, bool asserted) {
  if (psx::platform::latchIrqEdge(core.game->spu.irqLevel, IRQ_BIT_SPU, asserted) != 0u) {
    core.raiseInterrupt(IRQ_BIT_SPU);
  }
}
