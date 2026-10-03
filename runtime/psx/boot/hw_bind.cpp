// hw_bind.cpp — per-instance HW-peripheral state binders (SPU/MDEC/CD-controller/XA). Split out of
// native_boot.cpp (2026-07 restructure): grouped here so a reader can find every per-core HW bind in one
// place instead of buried mid-file in the boot driver.
#include "hw_bind.h"
#include "core.h"
#include "game.h"
#include "irq_edge.h"   // irq_latch_edge / IRQ_BIT_SPU — how a source's line becomes an I_STAT bit
#include <lucent/log.h> // diagnostics: lucent::debug (channel-gated internally — never guard it)

// Bind THIS core's per-instance SPU state (Beetle spu.c), lazily powering it on first use. Like gte_bind,
// called per core frame-step + at boot, from the explicit Core — two cores keep SEPARATE SPU state.
// Also binds this core's SPU write log so spu_write appends to A's or B's per-Core log during SBS, and
// this core as the target of the SPU's interrupt line (spu_irq_raise below).
void spu_bind(Core *c) {
  c->game->spu.bind(c);
}

// The SPU's interrupt line, arriving from spu_beetle.c's IRQ_Assert (which the vendored Beetle spu.c
// calls whenever SPUCNT bit 6 is set and an SPU-RAM access matches IRQAddr). Lives on the C++ side
// because raising it needs Game/Hle; spu_beetle.c is plain C and only holds the bound Core*.
//
// Two things are deliberately NOT done here. It does not decide whether the guest cares — I_MASK and
// the registered handler chain are Hle::irqPoll's business, and pre-filtering here would make "the
// game masks it" indistinguishable from "the framework dropped it". And it does not deliver: guest
// code may only run at a call-coherent boundary, which is what PW_IRQ arms.
extern "C" void spu_irq_raise(void *corev, int asserted) {
  Core *c = static_cast<Core *>(corev);
  Hle &hle = c->game->hle;
  const uint32_t before = hle.i_stat;
  hle.i_stat = irq_latch_edge(c->game->spu.irqLevel, hle.i_stat, IRQ_BIT_SPU, asserted != 0);
  if (hle.i_stat == before) {
    return; // level unchanged, or a falling edge: nothing new latched
  }
  c->pending_work |= Core::PW_IRQ; // arm the per-function-entry delivery gate
  lucent::debug("irq",
                "SPU raised IRQ9 -> I_STAT=0x{:03X} (mask=0x{:03X}, {})",
                hle.i_stat,
                hle.i_mask,
                (hle.i_mask & (1u << IRQ_BIT_SPU)) ? "unmasked" : "MASKED by the guest");
}
// Same for MDEC (per-instance; lazy power on first bind — MDEC has no separate global init).
void mdec_bind(Core *c) {
  c->game->mdec.bind();
}
// Bind THIS core's per-instance XA streamer (xa_stream.c) state. Unlike the CD controller (whose
// cdc_read/cdc_write now take the CdcState explicitly), the XA streamer still needs a bound instance:
// the vendored Beetle spu.c pulls samples through the context-free CDC_GetCDAudioSample(s32*)
// callback (sanctioned vendor interop). Same per-frame-step contract as gte/spu/mdec.
void xa_bind(Core *c) {
  xa_bind_state(&c->game->xa);
} // decls in xa_state.h  (via game.h, extern "C")
