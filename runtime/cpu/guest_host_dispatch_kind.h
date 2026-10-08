#pragma once

#include <cstdint>

namespace psx::cpu {

// What a guest address is when execution reaches it: ordinary guest code, a host service (a title
// override, a platform HLE leaf, a BIOS table entry), or an address no code image claims.
enum class GuestHostDispatchKind : std::uint8_t {
  ExecuteGuest,
  HostService,
  Fault,
};

} // namespace psx::cpu
