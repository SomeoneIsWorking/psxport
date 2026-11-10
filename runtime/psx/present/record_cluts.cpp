#include "record_cluts.h"

#include "vram_pixel.h"

#include <algorithm>
#include <array>

namespace psx::present {
namespace {

std::uint32_t clutKey(const DrawPrimitive &primitive) {
  return (static_cast<std::uint32_t>(primitive.state.texMode) << 16) | primitive.clutWord;
}

bool samplesClut(const DrawPrimitive &primitive) {
  return primitive.textured && primitive.state.texMode < 2;
}

std::uint32_t appendFromVram(FrameRecord &record, const DrawPrimitive &primitive, std::span<const std::uint16_t> vram) {
  const gpu::VramPoint at = gpu::decodeClut(primitive.clutWord);
  std::array<std::uint16_t, 256> entries{};
  const std::size_t count = gpu::clutEntryCount(static_cast<gpu::TexDepth>(primitive.state.texMode));
  for (std::size_t i = 0; i < count; i++) {
    entries[i] = gpu::vramHalfWord(vram, {at.x + static_cast<int>(i), at.y});
  }
  return record.appendClut(std::span<const std::uint16_t>(entries.data(), count));
}

} // namespace

void RecordCluts::index(const FrameRecord &shown) {
  offsets_.clear();
  for (const RecordEntry &entry : shown.entries()) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entry);
        primitive != nullptr && samplesClut(*primitive) && primitive->clutOffset != kNoClut) {
      offsets_.emplace_back(clutKey(*primitive), primitive->clutOffset);
    }
  }
  std::sort(offsets_.begin(), offsets_.end());
  offsets_.erase(std::unique(offsets_.begin(),
                             offsets_.end(),
                             [](const auto &a, const auto &b) {
                               return a.first == b.first;
                             }),
                 offsets_.end());
}

bool RecordCluts::resolve(FrameRecord &into, DrawPrimitive &primitive, std::span<const std::uint16_t> vram) {
  if (!samplesClut(primitive)) {
    return true;
  }
  const std::uint32_t key = clutKey(primitive);
  auto at = std::lower_bound(offsets_.begin(), offsets_.end(), key, [](const auto &entry, std::uint32_t k) {
    return entry.first < k;
  });
  if (at == offsets_.end() || at->first != key) {
    const std::uint32_t appended = appendFromVram(into, primitive, vram);
    if (appended == kNoClut) {
      return false;
    }
    at = offsets_.insert(at, {key, appended});
  }
  primitive.clutOffset = at->second;
  return true;
}

} // namespace psx::present
