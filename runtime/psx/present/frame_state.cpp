// frame_state.cpp — saved producer states by scope serial, collected per record.
#include "frame_state.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <variant>

namespace psx::present {

std::optional<std::span<const std::byte>> FrameState::find(ObjectId id) const {
  const std::uint64_t key = indexKey(id);
  const auto found = std::lower_bound(saved_.begin(), saved_.end(), key, [](const Saved &saved, std::uint64_t k) {
    return saved.key < k;
  });
  if (found == saved_.end() || found->key != key || found->ambiguous) {
    return std::nullopt;
  }
  return std::span<const std::byte>(bytes_).subspan(found->offset, found->size);
}

bool FrameState::holds(ObjectId id) const {
  const std::uint64_t key = indexKey(id);
  const auto found = std::lower_bound(saved_.begin(), saved_.end(), key, [](const Saved &saved, std::uint64_t k) {
    return saved.key < k;
  });
  return found != saved_.end() && found->key == key;
}

void stateSizeMismatch(std::size_t saved, std::size_t read) {
  lucent::error("present", "producer state saved as {} bytes read as {}", saved, read);
  std::abort();
}

const FrameStates::Saved *FrameStates::find(std::uint32_t serial) const {
  const auto found =
      std::lower_bound(bySerial_.begin(), bySerial_.end(), serial, [](const Saved &saved, std::uint32_t s) {
        return saved.serial < s;
      });
  return found != bySerial_.end() && found->serial == serial ? &*found : nullptr;
}

void FrameStates::save(const RecordKey &owner, std::span<const std::span<const std::byte>> parts) {
  const auto at =
      std::lower_bound(bySerial_.begin(), bySerial_.end(), owner.serial, [](const Saved &saved, std::uint32_t s) {
        return saved.serial < s;
      });
  if (at != bySerial_.end() && at->serial == owner.serial) {
    at->ambiguous = true;
    return;
  }
  Saved saved{.serial = owner.serial, .id = {owner.producer, owner.object}, .frame = frame_, .offset = bytes_.size()};
  for (const std::span<const std::byte> part : parts) {
    bytes_.insert(bytes_.end(), part.begin(), part.end());
  }
  saved.size = bytes_.size() - saved.offset;
  bySerial_.insert(at, saved);
}

void FrameStates::collect(const FrameRecord &record, FrameState &into) const {
  into.clear();
  if (bySerial_.empty()) {
    return;
  }
  // One note per keyed primitive, then one entry per object: ambiguous when its notes disagree.
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || !primitive->key) {
      continue;
    }
    const Saved *saved = find(primitive->key->serial);
    into.saved_.push_back({.key = FrameState::indexKey({primitive->key->producer, primitive->key->object}),
                           .serial = primitive->key->serial,
                           .offset = saved != nullptr ? saved->offset : 0,
                           .size = saved != nullptr ? saved->size : 0,
                           // Part of the object was drawn from a scope that saved nothing.
                           .ambiguous = saved == nullptr || saved->ambiguous});
  }
  std::sort(into.saved_.begin(), into.saved_.end(), [](const FrameState::Saved &a, const FrameState::Saved &b) {
    return a.key < b.key;
  });
  std::size_t kept = 0;
  for (std::size_t begin = 0; begin < into.saved_.size();) {
    FrameState::Saved object = into.saved_[begin];
    std::size_t end = begin + 1;
    for (; end < into.saved_.size() && into.saved_[end].key == object.key; ++end) {
      object.ambiguous = object.ambiguous || into.saved_[end].ambiguous || into.saved_[end].serial != object.serial;
    }
    if (!object.ambiguous) {
      const std::size_t from = object.offset;
      object.offset = into.bytes_.size();
      into.bytes_.insert(into.bytes_.end(),
                         bytes_.begin() + static_cast<std::ptrdiff_t>(from),
                         bytes_.begin() + static_cast<std::ptrdiff_t>(from + object.size));
    }
    into.saved_[kept++] = object;
    begin = end;
  }
  into.saved_.resize(kept);
}

void FrameStates::endFrame() {
  frame_++;
  spareBytes_.clear();
  std::size_t kept = 0;
  for (Saved &saved : bySerial_) {
    if (frame_ - saved.frame >= kRetainedFrames) {
      continue;
    }
    const std::size_t from = saved.offset;
    saved.offset = spareBytes_.size();
    spareBytes_.insert(spareBytes_.end(),
                       bytes_.begin() + static_cast<std::ptrdiff_t>(from),
                       bytes_.begin() + static_cast<std::ptrdiff_t>(from + saved.size));
    bySerial_[kept++] = saved;
  }
  bySerial_.resize(kept);
  std::swap(bytes_, spareBytes_);
}

void FrameStates::clear() {
  bySerial_.clear();
  bytes_.clear();
}

} // namespace psx::present
