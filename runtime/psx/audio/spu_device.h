// class SpuDevice: one Game's Beetle SPU state, its interrupt line and its debug counters.
#pragma once
#include "beetle_device_instance.h"
#include "spu_beetle.h"
#include "spu_state.h"
#include <array>
#include <cstdint>
#include <optional>
class Core;

class SpuDevice : public psx::platform::BeetleDeviceInstance<SPU_NewState, SPU_FreeState, SPU_BindState, SPU_Power> {
public:
  SpuDevice();

  // Asserted level of this instance's interrupt line; I_STAT is edge-latched from it (spu_device.cpp).
  uint32_t irqLevel = 0;
  psx::audio::spu::DebugCounters debug;
  // PSXPORT_SPUBT: the register offset whose writes print a host backtrace.
  std::optional<uint32_t> writeBacktraceOffset;
  // KON/KOFF register halves (0x188..0x18E) written since the SPU last ran a sample.
  std::array<uint16_t, 4> pendingKeys{};
  // The field the SPU is running through (spu_beetle.cpp): emulated CPU ticks at its start, the clocks its
  // cadence owes, and the clocks run so far, which accesses after the field's end carry into the next one.
  bool fieldOpen = false;
  bool fieldStarted = false;
  uint64_t fieldOrigin = 0;
  int32_t fieldBudget = 0;
  int32_t fieldRun = 0;
  // Beetle's clocks to its next sample as of the last run; 0 when unknown.
  int32_t untilSample = 0;

  // Make this instance the live SPU with `core` its interrupt target, powering it at first use.
  void bind(Core *core) {
    core_ = core;
    BeetleDeviceInstance::bind();
  }
  // Bind without powering, for the save-state owner.
  void bindExisting(Core *core) {
    core_ = core;
    BeetleDeviceInstance::bindExisting();
  }
  Core *core() const {
    return core_;
  }
  static SpuDevice *bound() {
    return static_cast<SpuDevice *>(boundInstance());
  }

private:
  Core *core_ = nullptr;
};
