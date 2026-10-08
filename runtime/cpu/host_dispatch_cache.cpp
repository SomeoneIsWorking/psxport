#include "host_dispatch_cache.h"

namespace psx::cpu {

HostDispatchVerdictCache::HostDispatchVerdictCache() : slots_(kSlotCount) {}

void HostDispatchVerdictCache::observe(const DispatchEpoch &epoch) {
  if (epoch == epoch_) {
    return;
  }
  epoch_ = epoch;
  ++stats_.flushes;
  // A generation is never reused, so no slot has to be touched to discard what it holds. The counter
  // wrapping would revive a slot from four billion flushes ago; clearing then keeps that impossible.
  if (++generation_ == 0) {
    slots_.assign(kSlotCount, Slot{});
    generation_ = 1;
  }
}

HostDispatchVerdictCache::Slot &HostDispatchVerdictCache::slotFor(std::uint32_t guestAddress) {
  return slots_[(guestAddress >> 2u) & (kSlotCount - 1u)];
}

std::optional<GuestHostDispatchKind> HostDispatchVerdictCache::find(std::uint32_t guestAddress) {
  const Slot &slot = slotFor(guestAddress);
  if (slot.generation != generation_ || slot.address != guestAddress) {
    ++stats_.misses;
    return std::nullopt;
  }
  ++stats_.hits;
  return slot.kind;
}

void HostDispatchVerdictCache::store(std::uint32_t guestAddress, GuestHostDispatchKind kind) {
  slotFor(guestAddress) = {guestAddress, generation_, kind};
}

} // namespace psx::cpu
