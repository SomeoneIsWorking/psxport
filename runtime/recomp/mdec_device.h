// mdec_device.h — class MdecDevice — RAII owner of one per-instance Beetle MDEC state (mirror of
// SpuDevice). One per Game so two cores keep SEPARATE MDEC state; bound per core frame-step via
// bind() (hw_bind.cpp).
#pragma once
#include "mdec_state.h"   // MDEC_NewState/FreeState/BindState/Power (C, mdec_beetle.c)

class MdecDevice {
public:
  void* state   = nullptr;  // per-instance MDEC state (Beetle mdec.c), heap-allocated
  int   powered = 0;        // MDEC_Power run on this instance's state yet? (lazy power on first bind)

  MdecDevice() { state = MDEC_NewState(); }
  ~MdecDevice() { MDEC_FreeState(state); }
  MdecDevice(const MdecDevice&) = delete;
  MdecDevice& operator=(const MdecDevice&) = delete;

  // Make this instance the active MDEC, lazily powering it on (MDEC has no separate global init).
  void bind() {
    MDEC_BindState(state);
    if (!powered) { MDEC_Power(); powered = 1; }
  }
};
