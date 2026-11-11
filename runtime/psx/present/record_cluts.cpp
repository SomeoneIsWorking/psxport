#include "record_cluts.h"

#include "blend.h"
#include "vram_pixel.h"

#include <lucent/log.h>

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

std::uint32_t channel(std::uint16_t entry, unsigned shift) {
  return (entry >> shift) & 0x1Fu;
}

// Each 5-bit channel blended, the STP bit the later entry's; 0x0000 is the transparent texel, so it is never
// blended into or out of, nor landed on.
std::uint16_t blendEntry(std::uint16_t from, std::uint16_t to, float t) {
  if (from == 0 || to == 0) {
    return to;
  }
  std::uint32_t blended = to & 0x8000u;
  for (const unsigned shift : {0u, 5u, 10u}) {
    const auto at = static_cast<std::uint32_t>(
        lerpInt(static_cast<std::int32_t>(channel(from, shift)), static_cast<std::int32_t>(channel(to, shift)), t));
    blended |= at << shift;
  }
  return blended == 0 ? to : static_cast<std::uint16_t>(blended);
}

} // namespace

const RecordCluts::Indexed *RecordCluts::find(std::span<const Indexed> offsets, std::uint32_t key) {
  const auto at = std::lower_bound(offsets.begin(), offsets.end(), key, [](const Indexed &entry, std::uint32_t k) {
    return entry.key < k;
  });
  return at != offsets.end() && at->key == key ? &*at : nullptr;
}

void RecordCluts::collect(const FrameRecord &record, std::vector<Indexed> &out) {
  out.clear();
  for (const RecordEntry &entry : record.entries()) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entry);
        primitive != nullptr && samplesClut(*primitive) && primitive->clutOffset != kNoClut) {
      out.push_back({clutKey(*primitive), primitive->clutOffset, false});
    }
  }
  std::sort(out.begin(), out.end(), [](const Indexed &a, const Indexed &b) {
    return a.key != b.key ? a.key < b.key : a.offset < b.offset;
  });
  out.erase(std::unique(out.begin(),
                        out.end(),
                        [](const Indexed &a, const Indexed &b) {
                          return a.key == b.key;
                        }),
            out.end());
}

void RecordCluts::index(const FrameRecord &shown) {
  collect(shown, offsets_);
  earlierOffsets_.clear();
  earlier_ = nullptr;
  cycles_ = nullptr;
  t_ = 1.0f;
}

void RecordCluts::index(const FrameRecord &shown, Between between) {
  collect(shown, offsets_);
  collect(between.earlier, earlierOffsets_);
  earlier_ = &between.earlier;
  cycles_ = &between.cycles;
  t_ = between.t;
}

bool RecordCluts::resolve(FrameRecord &into, DrawPrimitive &primitive, std::span<const std::uint16_t> vram) {
  if (!samplesClut(primitive)) {
    return true;
  }
  const std::uint32_t key = clutKey(primitive);
  auto at = std::lower_bound(offsets_.begin(), offsets_.end(), key, [](const Indexed &entry, std::uint32_t k) {
    return entry.key < k;
  });
  if (at == offsets_.end() || at->key != key) {
    const std::uint32_t appended = appendFromVram(into, primitive, vram);
    if (appended == kNoClut) {
      return false;
    }
    at = offsets_.insert(at, {key, appended, true});
  }
  if (!at->settled) {
    at->settled = true;
    const Indexed *before = earlier_ != nullptr ? find(earlierOffsets_, key) : nullptr;
    if (before != nullptr) {
      const std::size_t count = gpu::clutEntryCount(static_cast<gpu::TexDepth>(primitive.state.texMode));
      const std::span<const std::uint16_t> from = earlier_->clutPool().subspan(before->offset, count);
      const std::span<const std::uint16_t> to = into.clutPool().subspan(at->offset, count);
      const gpu::VramPoint row = gpu::decodeClut(primitive.clutWord);
      if (!std::equal(from.begin(), from.end(), to.begin()) && cycles_->steps(row.x, row.y, from, to)) {
        std::array<std::uint16_t, 256> entries{};
        for (std::size_t i = 0; i < count; i++) {
          entries[i] = blendEntry(from[i], to[i], t_);
        }
        const std::uint32_t blended = into.appendClut(std::span<const std::uint16_t>(entries.data(), count));
        if (blended == kNoClut) {
          return false;
        }
        at->offset = blended;
        lucent::debug("clut", "in-between at t={:.2f} cross-fades the cycled CLUT row ({}, {})", t_, row.x, row.y);
      }
    }
  }
  primitive.clutOffset = at->offset;
  return true;
}

} // namespace psx::present
