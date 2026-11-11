// pad_delivery.h — one pad frame's packet as the host hands it to the guest.
#pragma once

#include <array>
#include <cstdint>

namespace psx::input {

struct PadDelivery {
  // The standard digital-pad response: ID, data marker, then the active-low button bytes.
  std::array<std::uint8_t, 4> packet{};
  // Port 2's packet; a disconnected port 2 answers nothing.
  std::array<std::uint8_t, 4> slot1Packet{};
  bool slot1Connected = false;
};

// The controller ports a title can read; the host maps devices onto the ports the title declares.
inline constexpr std::uint32_t kPadPorts = 2;

} // namespace psx::input
