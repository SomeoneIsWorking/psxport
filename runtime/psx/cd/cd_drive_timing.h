// Setmode bits and the drive timing they select, in the per-Game emulated CPU clock.
#pragma once

#include "emulated_time.h"

#include <cstdint>

namespace psx::cd {

inline constexpr uint8_t kModeDoubleSpeed = 0x80u;
inline constexpr uint8_t kModeXaAdpcm = 0x40u;     // STRSND: XA audio sectors go to the SPU
inline constexpr uint8_t kModeWholeSector = 0x20u; // 2340 bytes from the header
inline constexpr uint8_t kModeSize2328 = 0x10u;    // 2328 bytes after the subheader; wins over kModeWholeSector
inline constexpr uint8_t kModeXaFilter = 0x08u;    // only the Setfilter file/channel reaches the SPU

constexpr int driveSectorsPerSecond(uint8_t mode) {
  return (mode & kModeDoubleSpeed) != 0 ? 150 : 75;
}

constexpr uint64_t driveSectorPeriodCpuTicks(uint8_t mode) {
  return psx::frame::kNominalPsxCpuHz / static_cast<uint64_t>(driveSectorsPerSecond(mode));
}

// Where one raw 2352-byte sector's FIFO bytes start and how many there are (Beetle cdc.c).
struct SectorFraming {
  uint32_t offset;
  uint32_t bytes;
};

constexpr SectorFraming sectorFraming(uint8_t mode) {
  if ((mode & kModeSize2328) != 0) {
    return {24u, 2328u};
  }
  if ((mode & kModeWholeSector) != 0) {
    return {12u, 2340u};
  }
  return {24u, 2048u};
}

} // namespace psx::cd
