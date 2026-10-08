// machine_snapshot — the guest-visible CPU, GTE, main-RAM and scratchpad state of one Core, captured
// and restored as a unit, for the per-function override differential (`override_differential.h`).
//
// It is NOT a savestate. Device models (GPU, SPU, CD, timers, DMA registers, interrupt latches) are not
// captured, and restoring a snapshot does not rewind them; the differential keeps devices consistent by
// journaling their traffic instead (`side_effect_journal.h`).
//
// RESTORE IS AN EXECUTABLE WRITE. Putting bytes back into RAM behind Lightrec's back would leave a block
// translated from the bytes being replaced reachable, so `restoreInto` writes only the ranges that
// differ and reports each one to the single invalidation owner (`notifyExecutableWrite`, source
// `Savestate`) after the bytes are visible.
#pragma once

#include "guest_program_image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class Core;

namespace psx::cpu {

inline constexpr std::uint32_t kMainRamBytes = 0x200000u;
inline constexpr std::uint32_t kScratchpadBytes = 0x400u;
inline constexpr std::uint32_t kScratchpadPhysicalBase = 0x1F800000u;
inline constexpr std::size_t kGteRegisterCount = 64;
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
  std::array<std::uint32_t, kGteRegisterCount> gte{};
  std::uint32_t gteFlags = 0;
  std::vector<std::uint8_t> ram;
  std::vector<std::uint8_t> scratchpad;
};

// Every maximal differing byte run between two snapshots' memories, in address order.
std::vector<ByteRange> differingRanges(const MachineSnapshot &a, const MachineSnapshot &b);

} // namespace psx::cpu
