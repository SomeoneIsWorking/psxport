// S1 stubs for runtime hooks not yet implemented (GTE, COP0, syscalls). These let the
// recompiled core compile and link so the emitter can be verified now; each is replaced
// by a real implementation in later stages (S2 HLE, S3 GTE).
#include "core.h"
#include "game.h"
#include <stdio.h>

// GTE (COP2) is the real Beetle implementation (gte_beetle.c). COP0 is minimal but no longer a
// STUB, and the difference mattered: `cop0_mtc` used to discard every write.
//
// COP0 register 12 is the Status register, and its bit 0 (IEc) is the CPU's MASTER INTERRUPT
// ENABLE. PSX code disables interrupts by clearing it and re-enables them by setting it — the BIOS's
// EnterCriticalSection/ExitCriticalSection are thin wrappers over exactly that, and library code
// frequently pokes SR directly instead of paying for the syscall. With the write discarded, the
// framework's `irq_enabled` could be cleared (by the syscall path) and then NEVER restored, because
// the guest's restore went through SR and evaporated. Result: interrupt delivery silently switched
// itself off for the rest of the run after the first critical section. Nothing reported it, because
// "no interrupt was pending" and "interrupts are disabled forever" look identical from outside.
//
// Registers are stored per-Core so two Cores never share exception state.
uint32_t cop0_mfc(Core *c, uint32_t reg) {
  return reg < 16 ? c->cop0[reg] : 0;
}

void cop0_mtc(Core *c, uint32_t reg, uint32_t v) {
  if (reg >= 16) {
    return;
  }
  c->cop0[reg] = v;
  if (reg == 12) {
    c->game->hle.irq_enabled = (v & 1u) ? 1 : 0; // SR.IEc — master interrupt enable
  }
}

// rec_syscall / rec_break live in hle.c (kernel concern).
