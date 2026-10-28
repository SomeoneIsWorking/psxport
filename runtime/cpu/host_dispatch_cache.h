#pragma once

#include "guest_host_dispatch_kind.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::cpu {

struct DispatchVerdict {
  GuestHostDispatchKind kind = GuestHostDispatchKind::ExecuteGuest;
  // The answer also reads live guest memory, so the owner re-derives it at every crossing (NativeDispatcher::classify).
  bool live = false;
};

// The answer to "what is this guest address" for every block boundary the executor crosses: a direct-mapped table
// of verdicts, each valid only under the DispatchEpoch it was stored with. Single-threaded, like the dispatcher.
class HostDispatchVerdictCache {
public:
  HostDispatchVerdictCache() : slots_(kSlotCount) {}

  std::optional<DispatchVerdict> find(std::uint32_t guestAddress, std::uint64_t epoch) const {
    const Slot &slot = slots_[slotIndex(guestAddress)];
    if (slot.epoch != epoch || slot.address != guestAddress) {
      return std::nullopt;
    }
    return slot.verdict;
  }
  void store(std::uint32_t guestAddress, std::uint64_t epoch, DispatchVerdict verdict) {
    slots_[slotIndex(guestAddress)] = {epoch, guestAddress, verdict};
  }
  // Drops one address's verdict, for an input change that can move no other address's answer.
  void forget(std::uint32_t guestAddress) {
    slots_[slotIndex(guestAddress)].epoch = 0;
  }

private:
  struct Slot {
    std::uint64_t epoch = 0; // never a live epoch, so a fresh slot is empty
    std::uint32_t address = 0;
    DispatchVerdict verdict;
  };

  static constexpr std::size_t kSlotCount = 1u << 14;

  static std::size_t slotIndex(std::uint32_t guestAddress) {
    return (guestAddress >> 2u) & (kSlotCount - 1u);
  }

  std::vector<Slot> slots_;
};

} // namespace psx::cpu
