#pragma once

#include "guest_host_dispatch_kind.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::cpu {

// Every input a dispatch verdict is derived from that changes only at a known point. A verdict stays
// valid exactly while every one of these is unchanged; the owner of each input advances its counter
// where it mutates (ImageCatalog::revision, NativeDispatcher::revision, PlatformHle::revision), so
// the cache never has to re-derive an address to learn that it is stale.
struct DispatchEpoch {
  std::uint64_t images = 0;    // which code images are resident, and which bytes each owns
  std::uint64_t overrides = 0; // which title overrides are installed, and which are suppressed now
  std::uint64_t services = 0;  // which platform HLE leaves are registered
  const void *game = nullptr;  // the Game whose service table the verdicts were read from

  friend bool operator==(const DispatchEpoch &lhs, const DispatchEpoch &rhs) {
    return lhs.images == rhs.images && lhs.overrides == rhs.overrides && lhs.services == rhs.services &&
           lhs.game == rhs.game;
  }
};

struct HostDispatchCacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t flushes = 0;
};

// The answer to "what is this guest address" for every block boundary the executor crosses.
//
// WHY IT EXISTS. The boundary callback runs for every executed block (364 million in a 1000-frame Toy
// Story 2 route) and asked the whole resolution chain each time: an image-catalog scan, a hash lookup
// of the override table, the HLE tables. Only ~9,650 distinct addresses are ever reached, and the
// answer for an address cannot change unless one of the inputs in `DispatchEpoch` does.
//
// A direct-mapped table of verdicts, flushed as a whole when the epoch moves. It stores only verdicts
// that are a function of the epoch; an address whose answer also depends on live guest memory is never
// stored (the owner decides, see NativeDispatcher::classify). Single-threaded, like the dispatcher.
class HostDispatchVerdictCache {
public:
  HostDispatchVerdictCache();

  // Declare the epoch the caller is about to read and store verdicts under. A different epoch from the
  // last one discards every stored verdict.
  void observe(const DispatchEpoch &epoch);

  std::optional<GuestHostDispatchKind> find(std::uint32_t guestAddress);
  void store(std::uint32_t guestAddress, GuestHostDispatchKind kind);

  const HostDispatchCacheStats &stats() const {
    return stats_;
  }

private:
  struct Slot {
    std::uint32_t address = 0;
    std::uint32_t generation = 0; // 0 is never a live generation, so a fresh slot is empty
    GuestHostDispatchKind kind = GuestHostDispatchKind::ExecuteGuest;
  };

  static constexpr std::size_t kSlotCount = 1u << 14;

  Slot &slotFor(std::uint32_t guestAddress);

  std::vector<Slot> slots_;
  DispatchEpoch epoch_;
  std::uint32_t generation_ = 1;
  HostDispatchCacheStats stats_;
};

} // namespace psx::cpu
