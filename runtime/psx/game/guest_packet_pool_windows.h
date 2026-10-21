// guest_packet_pool_windows.h — a title's packet pool, as GameRuntime::packetPool reports it.
#pragma once

#include "guest_address_range.h"

#include <array>
#include <cstdint>

class Core;

namespace psx::gpu {

// The pool's KSEG0 extent when it was read, and the guest words holding its live bounds.
struct PacketPoolExtent {
  std::array<std::uint32_t, 2> low{};
  std::array<std::uint32_t, 2> high{};
  std::uint32_t count = 0;
  std::array<std::uint32_t, 4> sources{};

  // Whether any byte of `store` (KSEG0) lies in a pool.
  bool overlaps(GuestSpan store) const;
  // Whether `store` rewrites a bound this extent was read from.
  bool readFrom(GuestSpan store) const;
};

// A title answers packetPool with the layout its engine uses. The layouts are not
// interchangeable: collapsing two separately allocated parity pools into one range would claim the
// gap between them, and splitting one window into halves would claim a top the guest never reaches.
struct GuestPacketPoolWindows {
  enum class Representation : std::uint8_t {
    // Two parity pools in one array: `base`, each pool `stride` bytes wide.
    FixedBaseStride,
    // Two separately allocated parity pools, each bounded by guest globals holding its live base and end.
    // A pair with both pointers 0 is absent; a single parity pool declares one pair.
    LiveBaseEndPointers,
    // One window [base, end) holding both parity ordering tables (C-12).
    SingleWindow,
  };

  Representation representation = Representation::FixedBaseStride;
  std::uint32_t base = 0;
  std::uint32_t stride = 0;
  std::uint32_t end = 0;
  std::array<std::uint32_t, 2> basePointer{};
  std::array<std::uint32_t, 2> endPointer{};

  // The pool now; live bounds are read from guest RAM.
  PacketPoolExtent extent(Core &core) const;
};

} // namespace psx::gpu
