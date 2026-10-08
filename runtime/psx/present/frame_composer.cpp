// frame_composer.cpp — replacing a produced object's entries with its render, by OT slot.
#include "frame_composer.h"

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace psx::present {
namespace {

std::uint64_t slotKey(OtSlot slot) {
  return (std::uint64_t{slot.table} << 32) | slot.index;
}

std::uint64_t objectKey(ObjectId id) {
  return (std::uint64_t{id.producer} << 32) | id.object;
}

std::uint32_t clutKey(const DrawPrimitive &primitive) {
  return (static_cast<std::uint32_t>(primitive.state.texMode) << 16) | primitive.clutWord;
}

bool samplesClut(const DrawPrimitive &primitive) {
  return primitive.textured && primitive.state.texMode < 2;
}

struct Covered {
  ObjectId id;
  const StateProducer *render = nullptr;
  std::span<const std::byte> to;
  std::vector<std::size_t> entries;
  std::unordered_map<std::uint64_t, std::size_t> firstInSlot;
};

class CollectingSink final : public PrimitiveSink {
public:
  void emit(OtSlot slot, const DrawPrimitive &primitive) override {
    emitted.emplace_back(slot, primitive);
  }
  std::vector<std::pair<OtSlot, DrawPrimitive>> emitted;
};

// Where a primitive in `slot` goes when no entry of the object was in it: the head of `slot` when the
// walk passed it, else before the first slot the walk reaches after it, else after the table's last
// entry. Nullopt when the record never walked the table.
class SlotPlacement {
public:
  explicit SlotPlacement(const FrameRecord &record) {
    for (const SlotStart &start : record.slotStarts()) {
      heads_.try_emplace(slotKey(start.slot), start.entry);
      starts_[start.slot.table].push_back(start);
    }
    const std::span<const RecordEntry> entries = record.entries();
    for (std::size_t i = 0; i < entries.size(); i++) {
      const auto *primitive = std::get_if<DrawPrimitive>(&entries[i]);
      if (primitive != nullptr && primitive->slot) {
        tableEnds_[primitive->slot->table] = i + 1;
      }
    }
  }

  std::optional<std::size_t> position(OtSlot slot) const {
    if (const auto head = heads_.find(slotKey(slot)); head != heads_.end()) {
      return head->second;
    }
    const auto starts = starts_.find(slot.table);
    if (starts == starts_.end()) {
      return std::nullopt;
    }
    for (const SlotStart &start : starts->second) {
      if (start.descending ? start.slot.index < slot.index : start.slot.index > slot.index) {
        return start.entry;
      }
    }
    const auto end = tableEnds_.find(slot.table);
    return end != tableEnds_.end() ? end->second : starts->second.back().entry;
  }

private:
  std::unordered_map<std::uint64_t, std::size_t> heads_;
  std::unordered_map<std::uint16_t, std::vector<SlotStart>> starts_;
  std::unordered_map<std::uint16_t, std::size_t> tableEnds_;
};

// The draw environment in effect where a rendered primitive lands, onto it.
void applyEnvironment(DrawPrimitive &primitive, const RecordDrawState &environment) {
  RecordDrawState &state = primitive.state;
  state.clipX0 = environment.clipX0;
  state.clipY0 = environment.clipY0;
  state.clipX1 = environment.clipX1;
  state.clipY1 = environment.clipY1;
  state.offsetX = environment.offsetX;
  state.offsetY = environment.offsetY;
  state.windowMaskX = environment.windowMaskX;
  state.windowMaskY = environment.windowMaskY;
  state.windowOffsetX = environment.windowOffsetX;
  state.windowOffsetY = environment.windowOffsetY;
  state.dither = environment.dither;
  state.maskSet = environment.maskSet;
  state.maskCheck = environment.maskCheck;
  state.skipRowParity = environment.skipRowParity;
  for (int i = 0; i < primitive.vertexCount; i++) {
    RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(i)];
    vertex.x += environment.offsetX;
    vertex.y += environment.offsetY;
  }
}

// The draw state in effect at each position: that of the first primitive at or after it, else the last one.
std::vector<const RecordDrawState *> environmentsAt(std::span<const RecordEntry> entries) {
  std::vector<const RecordDrawState *> at(entries.size() + 1, nullptr);
  const RecordDrawState *next = nullptr;
  for (std::size_t i = entries.size(); i-- > 0;) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entries[i])) {
      next = &primitive->state;
    }
    at[i] = next;
  }
  for (std::size_t i = 1; i < at.size(); i++) {
    if (at[i] == nullptr) {
      at[i] = at[i - 1];
    }
  }
  return at;
}

} // namespace

std::optional<FrameRecord> composeFrame(
    const FrameRecord &shown, const FrameState *from, const FrameState &to, float t, const StateProducers &producers) {
  if (producers.empty() || to.empty()) {
    return std::nullopt;
  }
  const std::span<const RecordEntry> entries = shown.entries();
  std::vector<Covered> covered;
  std::unordered_map<std::uint64_t, std::size_t> coveredIndex;
  std::unordered_map<std::uint32_t, std::uint32_t> clutOffsets;
  for (std::size_t i = 0; i < entries.size(); i++) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entries[i]);
    if (primitive == nullptr) {
      continue;
    }
    if (samplesClut(*primitive) && primitive->clutOffset != kNoClut) {
      clutOffsets.try_emplace(clutKey(*primitive), primitive->clutOffset);
    }
    if (!primitive->key) {
      continue;
    }
    const ObjectId id{primitive->key->producer, primitive->key->object};
    auto found = coveredIndex.find(objectKey(id));
    if (found == coveredIndex.end()) {
      const StateProducer *render = producers.find(id.producer);
      const std::optional<std::span<const std::byte>> state = render ? to.find(id) : std::nullopt;
      if (!state) {
        continue;
      }
      found = coveredIndex.emplace(objectKey(id), covered.size()).first;
      covered.push_back({id, render, *state, {}, {}});
    }
    Covered &object = covered[found->second];
    object.entries.push_back(i);
    if (primitive->slot) {
      object.firstInSlot.try_emplace(slotKey(*primitive->slot), i);
    }
  }
  if (covered.empty()) {
    return std::nullopt;
  }
  const SlotPlacement placement(shown);
  const std::vector<const RecordDrawState *> environments = environmentsAt(entries);

  bool replaced = false;
  std::vector<bool> removed(entries.size(), false);
  std::vector<std::vector<DrawPrimitive>> inserted(entries.size() + 1);
  for (const Covered &object : covered) {
    const std::optional<std::span<const std::byte>> earlier = from ? from->find(object.id) : std::nullopt;
    CollectingSink sink;
    object.render->render(earlier.value_or(object.to), object.to, earlier ? t : 1.0f, sink);
    std::vector<std::pair<std::size_t, DrawPrimitive>> placed;
    placed.reserve(sink.emitted.size());
    bool drawable = true;
    for (auto &[slot, primitive] : sink.emitted) {
      const auto own = object.firstInSlot.find(slotKey(slot));
      const std::optional<std::size_t> position =
          own != object.firstInSlot.end() ? std::optional<std::size_t>(own->second) : placement.position(slot);
      if (!position) {
        drawable = false;
        break;
      }
      if (samplesClut(primitive)) {
        const auto clut = clutOffsets.find(clutKey(primitive));
        if (clut == clutOffsets.end()) {
          drawable = false;
          break;
        }
        primitive.clutOffset = clut->second;
      }
      applyEnvironment(primitive, *environments[*position]);
      primitive.key = RecordKey{object.id.producer, object.id.object, 0, 0};
      primitive.slot = slot;
      placed.emplace_back(*position, primitive);
    }
    if (!drawable) {
      continue;
    }
    replaced = true;
    for (const std::size_t entry : object.entries) {
      removed[entry] = true;
    }
    for (auto &[position, primitive] : placed) {
      inserted[position].push_back(primitive);
    }
  }
  if (!replaced) {
    return std::nullopt;
  }

  std::vector<RecordEntry> composed;
  composed.reserve(entries.size());
  for (std::size_t i = 0; i <= entries.size(); i++) {
    for (DrawPrimitive &primitive : inserted[i]) {
      composed.emplace_back(primitive);
    }
    if (i < entries.size() && !removed[i]) {
      composed.push_back(entries[i]);
    }
  }
  FrameRecord out = shown;
  out.replaceEntries(std::move(composed));
  return out;
}

} // namespace psx::present
