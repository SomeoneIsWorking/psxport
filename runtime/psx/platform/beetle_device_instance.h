// One per-Game Beetle device state. Beetle keeps a single process-global bind point per device, so only the
// bound instance is live; an unpowered instance is bound without powering so a state save captures it as is.
#pragma once

namespace psx::platform {

template <void *(*NewState)(), void (*FreeState)(void *), void (*BindState)(void *), void (*Power)()>
class BeetleDeviceInstance {
public:
  BeetleDeviceInstance() : state_(NewState()) {}
  ~BeetleDeviceInstance() {
    FreeState(state_);
  }
  BeetleDeviceInstance(const BeetleDeviceInstance &) = delete;
  BeetleDeviceInstance &operator=(const BeetleDeviceInstance &) = delete;
  BeetleDeviceInstance(BeetleDeviceInstance &&) = delete;
  BeetleDeviceInstance &operator=(BeetleDeviceInstance &&) = delete;

  // Bind and power on at first use; the frame step makes the instance live this way.
  void bind() {
    bindExisting();
    if (!powered_) {
      Power();
      powered_ = true;
    }
  }

  void bindExisting() {
    BindState(state_);
    bound_ = this;
  }

  bool isBound() const {
    return bound_ == this;
  }

  // Returns the bind point to Beetle's shared default if this instance holds it.
  void release() {
    if (bound_ != this) {
      return;
    }
    BindState(nullptr);
    bound_ = nullptr;
  }

  // Resets this instance to power-on; it must be bound.
  void power() {
    Power();
    powered_ = true;
  }

protected:
  static BeetleDeviceInstance *boundInstance() {
    return bound_;
  }

private:
  inline static BeetleDeviceInstance *bound_ = nullptr;
  void *state_;
  bool powered_ = false;
};

} // namespace psx::platform
