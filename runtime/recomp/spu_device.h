// spu_device.h — class SpuDevice — RAII owner of one per-instance Beetle SPU state (spu.c keeps
// its struct layout private; we hold the opaque handle), plus the per-Game SBS write log. One per
// Game so two cores keep SEPARATE SPU state; bound per core frame-step via bind() (hw_bind.cpp).
#pragma once
#include "spu_state.h"   // SPU_NewState/FreeState/BindState/Power + spu_bind_log (C, spu_beetle.c)

class SpuDevice {
public:
  void* state    = nullptr;  // per-instance SPU state (Beetle spu.c), heap-allocated
  int   powered  = 0;        // SPU_Power run on this instance's state yet? (lazy power on first bind)
  void* writeLog = nullptr;  // per-Game SPU write log (SpuWriteLog*, spu_beetle.c) — Sbs compares A vs B
                             // at frame boundary to flag audio-relevant divergences (Issue #29). NULL when SBS off.

  SpuDevice() { state = SPU_NewState(); }
  ~SpuDevice() { SPU_FreeState(state); }
  SpuDevice(const SpuDevice&) = delete;
  SpuDevice& operator=(const SpuDevice&) = delete;

  // Make this instance the active SPU (and its write log the active log), lazily powering it on.
  void bind() {
    SPU_BindState(state);
    spu_bind_log(writeLog);   // NULL when SBS off — spu_write's null-check makes it a no-op
    if (!powered) { SPU_Power(); powered = 1; }
  }
};
