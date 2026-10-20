#pragma once

#include <cstdint>

namespace psx::cpu {

// A `jal` leaves the address after its delay slot in $ra.
inline constexpr std::uint32_t kJalReturnOffset = 8u;

// A jal site: the callee and the return address that ends the call.
struct GuestCallSite {
  std::uint32_t entry = 0;
  std::uint32_t returnPc = 0;

  // The call the `jal` at `jal` makes to `target`.
  static constexpr GuestCallSite fromJal(std::uint32_t jal, std::uint32_t target) {
    return GuestCallSite{target, jal + kJalReturnOffset};
  }
};

} // namespace psx::cpu
