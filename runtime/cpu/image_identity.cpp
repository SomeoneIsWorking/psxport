#include "image_identity.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace psx::cpu {

ImageIdentity ImageCatalog::activate(std::string_view name, GuestAddressRange range, std::uint64_t contentIdentity) {
  const ImageIdentity identity{nextId_++, nextGeneration_++};
  entries_.push_back({std::string(name), {range}, contentIdentity, identity, true});
  return identity;
}

std::size_t ImageCatalog::subtractRange(ImageIdentity identity, GuestAddressRange physicalRange) {
  if (!physicalRange.valid() || physicalRange.end > 0x20000000u) {
    throw std::invalid_argument("image range subtraction requires a valid physical range");
  }
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [identity](const Entry &candidate) {
    return candidate.active && candidate.identity == identity;
  });
  if (entry == entries_.end()) {
    return 0u;
  }
  std::vector<GuestAddressRange> surviving;
  surviving.reserve(entry->ranges.size() + 1u);
  for (const GuestAddressRange part : entry->ranges) {
    if (part.end <= physicalRange.begin || part.begin >= physicalRange.end) {
      surviving.push_back(part);
      continue;
    }
    if (part.begin < physicalRange.begin) {
      surviving.push_back({part.begin, physicalRange.begin});
    }
    if (physicalRange.end < part.end) {
      surviving.push_back({physicalRange.end, part.end});
    }
  }
  entry->ranges = std::move(surviving);
  entry->active = !entry->ranges.empty();
  return entry->ranges.size();
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
    if (!entry->active || std::none_of(entry->ranges.begin(), entry->ranges.end(), [physical](GuestAddressRange range) {
          return range.containsPhysical(physical);
        })) {
      continue;
    }
    return entry->identity;
  }
  return std::nullopt;
}

std::optional<ImageIdentity> ImageCatalog::resolve(GuestAddressRange physicalRange) const {
  if (!physicalRange.valid() || physicalRange.end > 0x20000000u) {
    return std::nullopt;
  }
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
    if (!entry->active) {
      continue;
    }
    for (const GuestAddressRange part : entry->ranges) {
      if (part.end <= physicalRange.begin || part.begin >= physicalRange.end) {
        continue;
      }
      // The newest overlap owns at least one requested byte. Unless one surviving fragment owns
      // all of them, the range crosses a residency boundary or an uncovered gap.
      if (part.begin <= physicalRange.begin && part.end >= physicalRange.end) {
        return entry->identity;
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::size_t ImageCatalog::activeCount() const {
  return std::count_if(entries_.begin(), entries_.end(), [](const Entry &entry) {
    return entry.active;
  });
}

} // namespace psx::cpu
