#pragma once

#include <cstdint>

namespace psx::cpu {

// A jal site: the callee and the return address that ends the call.
struct GuestCallSite {
  std::uint32_t entry = 0;
  std::uint32_t returnPc = 0;
};

} // namespace psx::cpu
