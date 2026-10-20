// guest_memory_map.h — the PSX physical memory map and the address folds over it.
#pragma once

#include <cstdint>
#include <optional>

namespace psx {

inline constexpr std::uint32_t kPhysicalMask = 0x1FFFFFFFu; // KUSEG/KSEG0/KSEG1 share one physical space
inline constexpr std::uint32_t kKseg0Base = 0x80000000u;

// 2 MiB of main RAM, mirrored four times through the low 8 MiB.
inline constexpr std::uint32_t kMainRamBytes = 0x200000u;
inline constexpr std::uint32_t kRamMirrorSpan = 0x800000u;
inline constexpr std::uint32_t kMainRamEnd = kKseg0Base + kMainRamBytes; // one past, as a KSEG0 address

inline constexpr std::uint32_t kScratchpadPhysicalBase = 0x1F800000u;
inline constexpr std::uint32_t kScratchpadBytes = 0x400u;
inline constexpr std::uint32_t kHardwareBase = 0x1F801000u;
inline constexpr std::uint32_t kHardwareBytes = 0x2000u;
inline constexpr std::uint32_t kBiosBase = 0x1FC00000u;
inline constexpr std::uint32_t kBiosBytes = 0x80000u;

constexpr std::uint32_t physical(std::uint32_t address) {
  return address & kPhysicalMask;
}

// The byte offset into main RAM of any of its segment or mirror names; nullopt outside main RAM.
constexpr std::optional<std::uint32_t> mainRamOffset(std::uint32_t address) {
  const std::uint32_t p = physical(address);
  if (p >= kRamMirrorSpan) {
    return std::nullopt;
  }
  return p & (kMainRamBytes - 1u);
}

// The KSEG0 name of a main-RAM address; any other address unsegmented.
constexpr std::uint32_t kseg0Address(std::uint32_t address) {
  const std::optional<std::uint32_t> offset = mainRamOffset(address);
  return offset.has_value() ? kKseg0Base | *offset : physical(address);
}

} // namespace psx
