// guest_address_range.h — a physical [begin, end) range of guest memory.
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
