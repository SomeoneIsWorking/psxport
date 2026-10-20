// Disc sector geometry and positions: BCD bytes, MSF and LBA, and which libcd commands carry one.
#pragma once

#include <array>
#include <cstdint>

namespace psx::cd {

// The first data sector is at 00:02:00, so an MSF carries a 150-sector lead-in.
inline constexpr int kLeadInSectors = 150;
inline constexpr int kSectorsPerSecond = 75;
inline constexpr int kSecondsPerMinute = 60;
inline constexpr std::uint32_t kRawSectorBytes = 2352u;

constexpr int fromBcd(std::uint8_t value) {
  return (value >> 4) * 10 + (value & 0x0F);
}

constexpr bool isBcd(std::uint8_t value) {
  return (value & 0x0Fu) <= 9 && (value >> 4) <= 9;
}

constexpr std::uint8_t toBcd(std::uint32_t value) {
  return static_cast<std::uint8_t>(((value / 10u) << 4u) | (value % 10u));
}

// BCD minute/second/frame to an LBA; a lead-in position is negative.
constexpr int msfToLba(std::uint8_t mm, std::uint8_t ss, std::uint8_t ff) {
  return (fromBcd(mm) * kSecondsPerMinute + fromBcd(ss)) * kSectorsPerSecond + fromBcd(ff) - kLeadInSectors;
}

// A sector count to BCD minute/second/frame, with no lead-in added.
constexpr std::array<std::uint8_t, 3> sectorsToMsf(std::uint32_t sectors) {
  return {toBcd(sectors / (kSecondsPerMinute * kSectorsPerSecond)),
          toBcd((sectors / kSectorsPerSecond) % kSecondsPerMinute),
          toBcd(sectors % kSectorsPerSecond)};
}

// An LBA at or after the lead-in to its absolute BCD minute/second/frame.
constexpr std::array<std::uint8_t, 3> lbaToMsf(int lba) {
  return sectorsToMsf(static_cast<std::uint32_t>(lba + kLeadInSectors));
}

// Whether libcd's CdControl sends an implicit Setloc for this command (Spyro 1 table at 0x80074DAC, psx issue 0115).
bool commandCarriesPosition(std::uint8_t command);

} // namespace psx::cd
