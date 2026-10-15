// Deterministic PlayStation CD drive timing, in the per-Game emulated CPU clock, so host load and pacing
// cannot change guest-visible callback ordering.
#pragma once

#include "emulated_time.h"

#include <cstdint>

namespace psx::cd {

constexpr int driveSectorsPerSecond(uint8_t mode) {
  return (mode & 0x80u) != 0 ? 150 : 75;
}

constexpr uint64_t driveSectorPeriodCpuTicks(uint8_t mode) {
  return psx::frame::kNominalPsxCpuHz / static_cast<uint64_t>(driveSectorsPerSecond(mode));
}

} // namespace psx::cd
