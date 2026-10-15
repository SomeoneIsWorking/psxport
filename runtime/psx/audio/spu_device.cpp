// spu_device.cpp — the SPU's interrupt line into the bound Core's I_STAT.
#include "spu_device.h"
#include "core.h"
#include "game.h"
#include "irq_edge.h"
#include <lucent/log.h>

// Whether the guest cares is Hle::irqPoll's business; delivery waits for the call-coherent boundary PW_IRQ arms.
void psx::audio::spu::raiseIrq(Core &core, bool asserted) {
  Core *c = &core;
  Hle &hle = c->game->hle;
  const uint32_t before = hle.i_stat;
  hle.i_stat |= psx::platform::latchIrqEdge(c->game->spu.irqLevel, IRQ_BIT_SPU, asserted);
  if (hle.i_stat == before) {
    return; // level unchanged, or a falling edge: nothing new latched
  }
  c->pending_work |= Core::PW_IRQ;
  lucent::debug("irq",
                "SPU raised IRQ9 -> I_STAT=0x{:03X} (mask=0x{:03X}, {})",
                hle.i_stat,
                hle.i_mask,
                (hle.i_mask & (1u << IRQ_BIT_SPU)) ? "unmasked" : "MASKED by the guest");
}
