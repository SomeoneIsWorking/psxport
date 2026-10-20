// store_watch — logs every guest store into one physical range (PSXPORT_CW or the REPL `watch`).
#pragma once

#include "guest_address_range.h"

#include <cstdint>

class Core;

namespace psx::debug {

class StoreWatch {
public:
  // Arms from PSXPORT_CW and its options.
  void configure();
  // Arms on `range`, masked to physical; an empty range disarms.
  void arm(GuestAddressRange range);
  bool armed() const {
    return end_ != 0;
  }
  // Called for each store while armed.
  void observe(Core &core, GuestStore store);
  std::uint32_t hits() const {
    return hits_;
  }

private:
  std::uint32_t begin_ = 0;
  std::uint32_t end_ = 0;
  std::uint32_t hits_ = 0;
  std::uint32_t limit_ = 0;
  bool registers_ = false;
  bool backtrace_ = false;
};

} // namespace psx::debug
