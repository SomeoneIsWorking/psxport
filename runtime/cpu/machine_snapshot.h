// machine_snapshot — CPU, GTE, RAM and scratchpad of one Core for the override differential; not a
// savestate (devices are journaled instead), and a restore invalidates every range it rewrites.
#pragma once

#include "guest_address_range.h"
#include "guest_memory_map.h"
#include "gte_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class Core;

namespace psx::cpu {

inline constexpr std::size_t kCop0RegisterCount = 16;

// A maximal run of differing bytes, as physical guest addresses: main RAM at [0, 2 MB) and the
// scratchpad at [0x1F800000, 0x1F800400).
using ByteRange = GuestAddressRange;

struct RestoreReport {
  std::size_t rangesWritten = 0;
  std::size_t bytesWritten = 0;
};

class MachineSnapshot {
public:
  static MachineSnapshot capture(const Core &core);
  // Writes this state into `core`; changed memory ranges are invalidated before return.
  RestoreReport restoreInto(Core &core) const;

  std::array<std::uint32_t, 32> gpr{};
  std::uint32_t hi = 0;
  std::uint32_t lo = 0;
  std::uint32_t pc = 0;
  std::array<std::uint32_t, kCop0RegisterCount> cop0{};
  GteRawState gte{};
  std::vector<std::uint8_t> ram;
  std::vector<std::uint8_t> scratchpad;
};

// Every maximal differing byte run between two snapshots' memories, in address order.
std::vector<ByteRange> differingRanges(const MachineSnapshot &a, const MachineSnapshot &b);

} // namespace psx::cpu
