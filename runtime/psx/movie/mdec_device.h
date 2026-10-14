// mdec_device.h — class MdecDevice — RAII owner of one per-instance Beetle MDEC state (mirror of
// SpuDevice). One per Game so two cores keep SEPARATE MDEC state; bound per core frame-step via
// bind() (hw_bind.cpp).
#pragma once
#include "mdec_state.h" // MDEC_NewState/FreeState/BindState/Power (C, mdec_beetle.c)

class MdecDevice {
public:
  void *state = nullptr; // per-instance MDEC state (Beetle mdec.c), heap-allocated
  int powered = 0;       // MDEC_Power run on this instance's state yet? (lazy power on first bind)

  MdecDevice() {
    state = MDEC_NewState();
  }
  ~MdecDevice() {
    MDEC_FreeState(state);
  }
  MdecDevice(const MdecDevice &) = delete;
  MdecDevice &operator=(const MdecDevice &) = delete;

  // Make this instance the active MDEC, lazily powering it on (MDEC has no separate global init).
  void bind() {
    bindExisting();
    if (!powered) {
      MDEC_Power();
      powered = 1;
    }
  }

  // Bind WITHOUT powering on — the mirror of SpuDevice::bindExisting, and for the same reason: the
  // save-state owner must be able to serialize an instance that was never powered on without
  // fabricating the state it is about to capture.
  void bindExisting() {
    MDEC_BindState(state);
    bound_ = this;
  }

  bool isBound() const {
    return bound_ == this;
  }

  // Returns the MDEC bind point to the shared default if this instance holds it.
  void release() {
    if (bound_ != this) {
      return;
    }
    MDEC_BindState(nullptr);
    bound_ = nullptr;
  }

private:
  // Beetle's bind point is process-global; this records which instance it names.
  inline static const MdecDevice *bound_ = nullptr;
};
