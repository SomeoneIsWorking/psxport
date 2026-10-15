// guest_address_range.h — a physical [begin, end) range of guest memory.
#pragma once

#include <cstdint>

// `bytes` of guest memory at an address.
struct GuestSpan {
  uint32_t address = 0;
  uint32_t bytes = 0;
};

// One guest store: its virtual address, the value and its width in bytes.
struct GuestStore {
  uint32_t address = 0;
  uint32_t value = 0;
  uint32_t width = 0;
};

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
