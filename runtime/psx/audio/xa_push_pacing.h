#pragma once

#include "spu_field_cadence.h"
#include "xa_adpcm.h"
#include "xa_state.h"

#include <cstdint>

// Push-mode XA consumption. The drive fills the ring at the disc rate; the SPU pulls at 44.1 kHz.
// Playback holds until a small backlog exists, then the rate is trimmed toward that backlog, so a
// stream starts at its true pitch and only clock drift between the two sides moves it.
class XaPushPacing {
public:
  static constexpr uint32_t kTargetBacklogFrames = 4096; // two stereo sectors, ~0.11 s at 37.8 kHz
  static constexpr uint32_t kHoldPulls = kTargetBacklogFrames * SpuFieldCadence::kSampleRateHz / psx::audio::kXaDefaultRateHz;
  static constexpr double kRateGain = 0.06;

  // A clip shorter than the backlog still plays once the hold has lasted as long as the backlog would.
  [[nodiscard]] static constexpr bool readyToPlay(uint32_t backlogFrames, uint32_t heldPulls) {
    return backlogFrames >= kTargetBacklogFrames || heldPulls >= kHoldPulls;
  }

  [[nodiscard]] static constexpr double rateScale(uint32_t backlogFrames) {
    const double error = (static_cast<double>(backlogFrames) - static_cast<double>(kTargetBacklogFrames)) /
                         static_cast<double>(psx::audio::kXaRingFrames);
    return 1.0 + error * kRateGain;
  }
};
