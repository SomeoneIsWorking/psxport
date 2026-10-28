// frame_record.cpp — FrameRecord storage.
#include "frame_record.h"

#include <lucent/log.h>

#include <utility>

namespace psx::present {

std::span<const std::uint16_t> FrameRecord::clut(const DrawPrimitive &primitive) const {
  if (primitive.clutOffset == kNoClut) {
    return {};
  }
  const std::size_t count = primitive.state.texMode == 0 ? 16u : 256u;
  if (primitive.clutOffset + count > clutPool_.size()) {
    return {};
  }
  return std::span<const std::uint16_t>(clutPool_).subspan(primitive.clutOffset, count);
}

std::span<const std::uint16_t> FrameRecord::pixels(const VramUpload &upload) const {
  const std::size_t count = static_cast<std::size_t>(upload.width) * static_cast<std::size_t>(upload.height);
  if (upload.pixelOffset + count > uploadPixels_.size()) {
    return {};
  }
  return std::span<const std::uint16_t>(uploadPixels_).subspan(upload.pixelOffset, count);
}

void FrameRecord::reset(std::uint64_t sequence, bool complete) {
  sequence_ = sequence;
  complete_ = complete;
  entries_.clear();
  clutPool_.clear();
  uploadPixels_.clear();
  slotStarts_.clear();
  drawEntryField_.reset();
}

void FrameRecord::append(const RecordEntry &entry) {
  if (!complete_) {
    return;
  }
  if (entries_.size() >= kMaxEntries) {
    lucent::warn("record", "record {} reached {} entries; it is marked incomplete", sequence_, kMaxEntries);
    markIncomplete();
    return;
  }
  entries_.push_back(entry);
}

std::optional<int> FrameRecord::undrawnRowParity() const {
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
    int parity = -1;
    if (const auto *primitive = std::get_if<DrawPrimitive>(&*entry)) {
      parity = primitive->state.skipRowParity;
    } else if (const auto *fill = std::get_if<VramFill>(&*entry)) {
      parity = fill->skipRowParity;
    } else {
      continue;
    }
    return parity >= 0 ? std::optional<int>(parity) : std::nullopt;
  }
  return std::nullopt;
}

RecordEntry *FrameRecord::last() {
  return entries_.empty() ? nullptr : &entries_.back();
}

void FrameRecord::dropLast() {
  if (!entries_.empty()) {
    entries_.pop_back();
  }
}

std::uint32_t FrameRecord::appendClut(std::span<const std::uint16_t> entries) {
  if (!complete_) {
    return kNoClut;
  }
  if (clutPool_.size() + entries.size() > kMaxPoolHalfwords) {
    lucent::warn(
        "record", "record {} CLUT pool reached {} halfwords; it is marked incomplete", sequence_, kMaxPoolHalfwords);
    markIncomplete();
    return kNoClut;
  }
  const auto offset = static_cast<std::uint32_t>(clutPool_.size());
  clutPool_.insert(clutPool_.end(), entries.begin(), entries.end());
  return offset;
}

void FrameRecord::appendUpload(VramUpload upload, std::span<const std::uint16_t> pixels) {
  if (!complete_) {
    return;
  }
  if (uploadPixels_.size() + pixels.size() > kMaxPoolHalfwords) {
    lucent::warn(
        "record", "record {} upload pool reached {} halfwords; it is marked incomplete", sequence_, kMaxPoolHalfwords);
    markIncomplete();
    return;
  }
  upload.pixelOffset = static_cast<std::uint32_t>(uploadPixels_.size());
  uploadPixels_.insert(uploadPixels_.end(), pixels.begin(), pixels.end());
  append(upload);
}

void FrameRecord::markIncomplete() {
  if (!complete_) {
    return;
  }
  complete_ = false;
  // An incomplete record is never replayed, so its contents need not be kept.
  entries_.clear();
  entries_.shrink_to_fit();
  clutPool_.clear();
  clutPool_.shrink_to_fit();
  uploadPixels_.clear();
  uploadPixels_.shrink_to_fit();
  slotStarts_.clear();
  slotStarts_.shrink_to_fit();
}

void FrameRecord::beginSlot(OtSlot slot, bool descending) {
  if (!complete_) {
    return;
  }
  slotStarts_.push_back({slot, entries_.size(), descending});
}

void FrameRecord::replaceEntries(std::span<const RecordEntry> entries) {
  if (!complete_) {
    return;
  }
  entries_.assign(entries.begin(), entries.end());
  slotStarts_.clear();
}

void FrameRecord::assignPools(const FrameRecord &other) {
  sequence_ = other.sequence_;
  complete_ = other.complete_;
  entries_.clear();
  clutPool_.assign(other.clutPool_.begin(), other.clutPool_.end());
  uploadPixels_.assign(other.uploadPixels_.begin(), other.uploadPixels_.end());
  slotStarts_.clear();
}

} // namespace psx::present
