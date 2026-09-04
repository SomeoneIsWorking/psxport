#include "image_identity.h"

#include <algorithm>

namespace psx::cpu {

ImageIdentity ImageCatalog::activate(std::string_view name, GuestAddressRange range, std::uint64_t contentIdentity) {
  const ImageIdentity identity{nextId_++, nextGeneration_++};
  entries_.push_back({std::string(name), range, contentIdentity, identity, true});
  return identity;
}

bool ImageCatalog::deactivate(ImageIdentity identity) {
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [identity](const Entry &candidate) {
    return candidate.active && candidate.identity == identity;
  });
  if (entry == entries_.end()) {
    return false;
  }
  entry->active = false;
  return true;
}

std::optional<ImageIdentity> ImageCatalog::resolve(std::uint32_t guestAddress) const {
  const std::uint32_t physical = guestAddress & 0x1fffffffu;
  // Later activations have residency precedence. This allows a relocatable
  // module to cover part of a still-resident executable and makes unload
  // reveal the underlying image again without address-only collisions.
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
    if (!entry->active || !entry->range.containsPhysical(physical)) {
      continue;
    }
    return entry->identity;
  }
  return std::nullopt;
}

std::size_t ImageCatalog::activeCount() const {
  return std::count_if(entries_.begin(), entries_.end(), [](const Entry &entry) {
    return entry.active;
  });
}

} // namespace psx::cpu
