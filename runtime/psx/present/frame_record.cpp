// frame_record.cpp — FrameRecord storage.
#include "frame_record.h"

#include <cmath>
#include <utility>

namespace psx::present {
namespace {

void placeAxis(int &whole, float &fraction, float position, PrimitiveKind kind) {
  if (kind == PrimitiveKind::Polygon) {
    const float below = std::floor(position);
    whole = static_cast<int>(below);
    fraction = position - below;
    return;
  }
  whole = static_cast<int>(std::floor(position + 0.5f));
  fraction = 0.0f;
}

} // namespace

void placeVertex(RecordVertex &vertex, float x, float y, PrimitiveKind kind) {
  placeAxis(vertex.x, vertex.subX, x, kind);
  placeAxis(vertex.y, vertex.subY, y, kind);
}

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

void FrameRecord::append(const RecordEntry &entry) {
  if (!complete_) {
    return;
  }
  if (entries_.size() >= kMaxEntries) {
    markIncomplete();
    return;
  }
  entries_.push_back(entry);
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

void FrameRecord::replaceEntries(std::vector<RecordEntry> entries) {
  if (!complete_) {
    return;
  }
  entries_ = std::move(entries);
  slotStarts_.clear();
}

} // namespace psx::present
