// frame_state.cpp — saved producer states by scope serial, collected per record.
#include "frame_state.h"

#include <lucent/log.h>

#include <cstdlib>
#include <variant>

namespace psx::present {

std::optional<std::span<const std::byte>> FrameState::find(ObjectId id) const {
  const auto found = saved_.find(indexKey(id));
  if (found == saved_.end() || found->second.ambiguous) {
    return std::nullopt;
  }
  return std::span<const std::byte>(bytes_).subspan(found->second.offset, found->second.size);
}

void FrameState::add(ObjectId id, std::uint32_t serial, std::span<const std::byte> state, bool ambiguous) {
  const auto [found, added] = saved_.try_emplace(indexKey(id), Saved{serial, bytes_.size(), state.size(), ambiguous});
  if (!added) {
    found->second.ambiguous = found->second.ambiguous || ambiguous || found->second.serial != serial;
    return;
  }
  bytes_.insert(bytes_.end(), state.begin(), state.end());
}

void stateSizeMismatch(std::size_t saved, std::size_t read) {
  lucent::error("present", "producer state saved as {} bytes read as {}", saved, read);
  std::abort();
}

void FrameStates::save(const RecordKey &owner, std::span<const std::byte> state) {
  const auto [found, added] =
      bySerial_.try_emplace(owner.serial, Saved{{owner.producer, owner.object}, frame_, false, {}});
  if (!added) {
    found->second.ambiguous = true;
    return;
  }
  found->second.bytes.assign(state.begin(), state.end());
}

FrameState FrameStates::collect(const FrameRecord &record) const {
  FrameState collected;
  if (bySerial_.empty()) {
    return collected;
  }
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || !primitive->key) {
      continue;
    }
    const ObjectId id{primitive->key->producer, primitive->key->object};
    const auto saved = bySerial_.find(primitive->key->serial);
    if (saved == bySerial_.end()) {
      // Part of the object was drawn from a scope that saved nothing.
      collected.add(id, primitive->key->serial, {}, true);
      continue;
    }
    collected.add(id, primitive->key->serial, saved->second.bytes, saved->second.ambiguous);
  }
  return collected;
}

void FrameStates::endFrame() {
  frame_++;
  std::erase_if(bySerial_, [this](const auto &entry) {
    return frame_ - entry.second.frame >= kRetainedFrames;
  });
}

void FrameStates::clear() {
  bySerial_.clear();
}

} // namespace psx::present
