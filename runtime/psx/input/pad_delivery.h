// pad_delivery.h — one pad frame's packet as the host hands it to the guest.
#pragma once

#include <array>
#include <cstdint>

namespace psx::input {

struct PadDelivery {
  // The standard digital-pad response: ID, data marker, then the active-low button bytes.
  std::array<std::uint8_t, 4> packet{};
  bool slot1Connected = false;
};

} // namespace psx::input
