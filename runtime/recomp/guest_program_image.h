// guest_program_image.h — immutable executable-image facts consumed by generic boot and routing.
#pragma once

#include <cstdint>

struct GuestAddressRange {
  uint32_t begin = 0;
  uint32_t end = 0;

  bool empty() const {
    return end == 0;
  }

  bool valid() const {
    return begin < end;
  }

  bool containsPhysical(uint32_t address) const {
    const uint32_t physical = address & 0x1FFFFFFFu;
    return valid() && physical >= begin && physical < end;
  }
};

// One game-owned value describes the resident executable image. Framework algorithms consume this
// type directly instead of reaching through the deprecated GameConfig bag. Addresses remain zero
// when not reverse-engineered; each consumer owns its honest refusal or explicit absence semantics.
struct GuestProgramImage {
  GuestAddressRange bss;
  uint32_t stackTopWordAddress = 0;
  uint32_t stackReserveWordAddress = 0;
  uint32_t heapBase = 0;
  uint32_t heapSizeStoreAddress = 0;
  uint32_t heapBaseStoreAddress = 0;
  uint32_t globalPointer = 0;
  uint32_t libcInitEntry = 0;
  uint32_t gameMainEntry = 0;
  uint32_t crt0Entry = 0;

  // Physical [begin,end) range of the resident recompiled text. The overlay router uses this to
  // distinguish MAIN from overlay modules.
  GuestAddressRange residentText;

  // Optional wider physical code range for the diagnostic stack heuristic. An empty range means
  // residentText; this fallback lives here so every caller uses one rule.
  GuestAddressRange backtraceText;

  struct StackBias {
    bool declared = false;
    int32_t bytes = 0;
  } stackBias;

  GuestAddressRange effectiveBacktraceText() const {
    return backtraceText.empty() ? residentText : backtraceText;
  }
};
