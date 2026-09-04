// spu_device.h — class SpuDevice — RAII owner of one per-instance Beetle SPU state (spu.c keeps
// its struct layout private; we hold the opaque handle), plus the per-Game SBS write log. One per
// Game so two cores keep SEPARATE SPU state; bound per core frame-step via bind() (hw_bind.cpp).
#pragma once
#include "spu_state.h" // SPU_NewState/FreeState/BindState/Power + spu_bind_log (C, spu_beetle.c)
#include <cstdint>
class Core;

class SpuDevice {
public:
  void *state = nullptr;    // per-instance SPU state (Beetle spu.c), heap-allocated
  int powered = 0;          // SPU_Power run on this instance's state yet? (lazy power on first bind)
  void *writeLog = nullptr; // per-Game SPU write log (SpuWriteLog*, spu_beetle.c) — Sbs compares A vs B
                            // at frame boundary to flag audio-relevant divergences (Issue #29). NULL when SBS off.
  // Asserted LEVEL of this instance's interrupt lines (only bit IRQ_BIT_SPU is driven here). I_STAT is
  // edge-latched from it — see irq_edge.h and spu_irq_raise (hw_bind.cpp). Per-Game, so two cores keep
  // separate interrupt lines exactly as they keep separate SPU state.
  uint32_t irqLevel = 0;

  SpuDevice() {
    state = SPU_NewState();
  }
  ~SpuDevice() {
    SPU_FreeState(state);
  }
  SpuDevice(const SpuDevice &) = delete;
  SpuDevice &operator=(const SpuDevice &) = delete;

  // Make this instance the active SPU (its write log the active log, its Core the IRQ-line target),
  // lazily powering it on.
  void bind(Core *c) {
    SPU_BindState(state);
    spu_bind_log(writeLog); // NULL when SBS off — spu_write's null-check makes it a no-op
    spu_bind_irq_core(c);   // the SPU IRQ raises on THIS core's I_STAT
    if (!powered) {
      SPU_Power();
      powered = 1;
    }
  }
};
