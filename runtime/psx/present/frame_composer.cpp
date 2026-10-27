// frame_composer.cpp — replacing a produced object's entries with its render, by OT slot.
#include "frame_composer.h"

#include "vram_pixel.h"

#include <lucent/log.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace psx::present {
namespace {

constexpr lucent::Channel composeChannel{"compose"};

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

// The CLUT a primitive samples, read from `vram` into `record`'s pool; kNoClut when the pool is full.
std::uint32_t
appendClutFromVram(FrameRecord &record, const DrawPrimitive &primitive, std::span<const std::uint16_t> vram) {
  const gpu::VramPoint at = gpu::decodeClut(primitive.clutWord);
  std::array<std::uint16_t, 256> entries{};
  const std::size_t count = gpu::clutEntryCount(static_cast<gpu::TexDepth>(primitive.state.texMode));
  for (std::size_t i = 0; i < count; i++) {
    entries[i] = gpu::vramHalfWord(vram, {at.x + static_cast<int>(i), at.y});
  }
  return record.appendClut(std::span<const std::uint16_t>(entries.data(), count));
}

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
  state.maskSet = environment.maskSet;
  state.maskCheck = environment.maskCheck;
  state.skipRowParity = environment.skipRowParity;
  for (int i = 0; i < primitive.vertexCount; i++) {
    RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(i)];
    vertex.x += environment.offsetX;
    vertex.y += environment.offsetY;
  }
}

constexpr std::uint64_t kNoSlot = ~std::uint64_t{0};

} // namespace

bool FrameComposer::index(const FrameRecord &shown,
                          const FrameState &to,
                          const StateProducers &producers,
                          bool inBetween) {
  const std::span<const RecordEntry> entries = shown.entries();
  baked_.clear();
  for (const RecordEntry &entry : entries) {
    if (const auto *upload = std::get_if<VramUpload>(&entry); upload != nullptr && upload->key) {
      baked_.push_back(objectKey({upload->key->producer, upload->key->object}));
    }
  }
  std::sort(baked_.begin(), baked_.end());
  clutOffsets_.clear();
  keyed_.clear();
  tableEnds_.clear();
  for (std::size_t i = 0; i < entries.size(); i++) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entries[i]);
    if (primitive == nullptr) {
      continue;
    }
    if (primitive->slot) {
      const auto end = std::find_if(tableEnds_.begin(), tableEnds_.end(), [primitive](const auto &tableEnd) {
        return tableEnd.first == primitive->slot->table;
      });
      if (end != tableEnds_.end()) {
        end->second = i + 1;
      } else {
        tableEnds_.emplace_back(primitive->slot->table, i + 1);
      }
    }
    if (samplesClut(*primitive) && primitive->clutOffset != kNoClut) {
      clutOffsets_.emplace_back(clutKey(*primitive), primitive->clutOffset);
    }
    if (!primitive->key) {
      continue;
    }
    const std::uint64_t object = objectKey({primitive->key->producer, primitive->key->object});
    if (std::binary_search(baked_.begin(), baked_.end(), object)) {
      if (inBetween) {
        lucent::debug(composeChannel,
                      "record={} in-between shown whole: object {:#010x}/{:#010x} baked into an upload",
                      shown.sequence(),
                      primitive->key->producer,
                      primitive->key->object);
        refusal_ = {HeldCause::None, primitive->key->producer, true, i};
        return false;
      }
      continue;
    }
    keyed_.push_back({object, primitive->slot ? slotKey(*primitive->slot) : kNoSlot, i});
  }
  // The first offset pooled for a CLUT, the lowest, is the one the composed frame samples.
  std::sort(clutOffsets_.begin(), clutOffsets_.end());
  clutOffsets_.erase(std::unique(clutOffsets_.begin(),
                                 clutOffsets_.end(),
                                 [](const auto &a, const auto &b) {
                                   return a.first == b.first;
                                 }),
                     clutOffsets_.end());

  std::sort(keyed_.begin(), keyed_.end(), [](const Keyed &a, const Keyed &b) {
    return std::tie(a.object, a.slot, a.entry) < std::tie(b.object, b.slot, b.entry);
  });
  covered_.clear();
  for (std::size_t begin = 0; begin < keyed_.size();) {
    std::size_t end = begin + 1;
    std::size_t firstEntry = keyed_[begin].entry;
    while (end < keyed_.size() && keyed_[end].object == keyed_[begin].object) {
      firstEntry = std::min(firstEntry, keyed_[end].entry);
      ++end;
    }
    const ObjectId id{static_cast<std::uint32_t>(keyed_[begin].object >> 32),
                      static_cast<std::uint32_t>(keyed_[begin].object)};
    const StateProducer *render = producers.find(id.producer);
    const std::optional<std::span<const std::byte>> state = render ? to.find(id) : std::nullopt;
    if (state) {
      covered_.push_back({id, render, *state, begin, end, firstEntry});
    } else if (inBetween) {
      lucent::debug(composeChannel,
                    "record={} in-between shown whole: object {:#010x}/{:#010x} has no render or state",
                    shown.sequence(),
                    id.producer,
                    id.object);
      refusal_ = {HeldCause::NoState, id.producer, false, firstEntry};
      return false;
    }
    begin = end;
  }
  // Objects render in the order the record first drew them.
  std::sort(covered_.begin(), covered_.end(), [](const Covered &a, const Covered &b) {
    return a.firstEntry < b.firstEntry;
  });
  return true;
}

bool FrameComposer::slotPosition(const FrameRecord &shown, OtSlot slot, std::size_t &position) const {
  const std::span<const SlotStart> starts = shown.slotStarts();
  const auto head =
      std::lower_bound(heads_.begin(), heads_.end(), std::pair<std::uint64_t, std::size_t>{slotKey(slot), 0});
  if (head != heads_.end() && head->first == slotKey(slot)) {
    position = starts[head->second].entry;
    return true;
  }
  const auto first = std::partition_point(tableStarts_.begin(), tableStarts_.end(), [&](std::size_t i) {
    return starts[i].slot.table < slot.table;
  });
  const auto last = std::partition_point(first, tableStarts_.end(), [&](std::size_t i) {
    return starts[i].slot.table == slot.table;
  });
  if (first == last) {
    return false;
  }
  for (auto at = first; at != last; ++at) {
    const SlotStart &start = starts[*at];
    if (start.descending ? start.slot.index < slot.index : start.slot.index > slot.index) {
      position = start.entry;
      return true;
    }
  }
  const auto end = std::find_if(tableEnds_.begin(), tableEnds_.end(), [slot](const auto &tableEnd) {
    return tableEnd.first == slot.table;
  });
  position = end != tableEnds_.end() ? end->second : starts[*(last - 1)].entry;
  return true;
}

std::uint32_t *FrameComposer::clutOffset(std::uint32_t key) {
  const auto at =
      std::lower_bound(clutOffsets_.begin(), clutOffsets_.end(), key, [](const auto &entry, std::uint32_t k) {
        return entry.first < k;
      });
  return at != clutOffsets_.end() && at->first == key ? &at->second : nullptr;
}

FrameRecord &FrameComposer::output(const FrameRecord &shown) {
  if (!outputPooled_) {
    output_.assignPools(shown);
    outputPooled_ = true;
  }
  return output_;
}

std::span<std::byte> FrameComposer::resting(std::size_t bytes) {
  resting_.resize(bytes);
  return resting_;
}

FrameComposer::Placement FrameComposer::place(const FrameRecord &shown,
                                              ObjectId id,
                                              const StateProducer &render,
                                              const StateInterval &states,
                                              std::span<const Keyed> run,
                                              std::span<const std::uint16_t> vram,
                                              bool refuses) {
  const StateProducer::Composition composition = render.composition(states);
  if (composition == StateProducer::Composition::Recorded) {
    return Placement::Kept;
  }
  if (composition == StateProducer::Composition::Refused) {
    if (refuses) {
      lucent::debug(composeChannel,
                    "record={} in-between shown whole: object {:#010x}/{:#010x} refused t={}",
                    shown.sequence(),
                    id.producer,
                    id.object,
                    states.t);
      refusal_ = {HeldCause::ProducerRefused, id.producer, false, run.empty() ? SIZE_MAX : run.front().entry};
      return Placement::Refused;
    }
    return Placement::Kept;
  }
  sink_.clear();
  render.render(states, sink_);
  placed_.clear();
  emitted_.clear();
  for (auto &[slot, primitive] : sink_.primitives) {
    // The n-th primitive in a slot lands where the object's n-th entry there was, so the draw state
    // between them is the one the guest drew under; beyond its last entry, after it.
    const std::uint64_t key = slotKey(slot);
    auto count = std::find_if(emitted_.begin(), emitted_.end(), [key](const auto &e) {
      return e.first == key;
    });
    if (count == emitted_.end()) {
      emitted_.emplace_back(key, 0);
      count = emitted_.end() - 1;
    }
    const std::size_t nth = count->second++;
    const auto own = std::equal_range(run.begin(), run.end(), Keyed{0, key, 0}, [](const Keyed &a, const Keyed &b) {
      return a.slot < b.slot;
    });
    std::size_t position = 0;
    bool drawable = true;
    if (own.first != own.second) {
      const auto inSlot = static_cast<std::size_t>(own.second - own.first);
      position = own.first[static_cast<std::ptrdiff_t>(std::min(nth, inSlot - 1))].entry;
    } else if (!slotPosition(shown, slot, position)) {
      drawable = false;
    }
    if (drawable && samplesClut(primitive)) {
      std::uint32_t *offset = clutOffset(clutKey(primitive));
      if (offset == nullptr) {
        const std::uint32_t appended = appendClutFromVram(output(shown), primitive, vram);
        if (appended == kNoClut) {
          drawable = false;
        } else {
          const auto at = std::lower_bound(
              clutOffsets_.begin(), clutOffsets_.end(), clutKey(primitive), [](const auto &entry, std::uint32_t k) {
                return entry.first < k;
              });
          offset = &clutOffsets_.insert(at, {clutKey(primitive), appended})->second;
        }
      }
      if (offset != nullptr) {
        primitive.clutOffset = *offset;
      }
    }
    if (!drawable) {
      if (refuses) {
        lucent::debug(composeChannel,
                      "record={} in-between shown whole: object {:#010x}/{:#010x} has no place or CLUT",
                      shown.sequence(),
                      id.producer,
                      id.object);
        refusal_ = {HeldCause::Unplaceable, id.producer, false, run.empty() ? SIZE_MAX : run.front().entry};
        return Placement::Refused;
      }
      return Placement::Kept;
    }
    applyEnvironment(primitive, *environments_[position]);
    primitive.key = RecordKey{id.producer, id.object, 0, 0};
    primitive.slot = slot;
    placed_.emplace_back(position, primitive);
  }
  return Placement::Drawn;
}

const FrameRecord *FrameComposer::compose(const FrameRecord &shown,
                                          const FrameState *from,
                                          const FrameState &to,
                                          float t,
                                          const StateProducers &producers,
                                          std::span<const std::uint16_t> vram) {
  refusal_ = {};
  if (to.empty()) {
    lucent::debug(composeChannel, "record={} shown whole: no state saved", shown.sequence());
    return nullptr;
  }
  // An in-between keeping an owned object as S drew it would mix two moments: it refuses instead.
  const bool inBetween = from != nullptr && t < 1.0f;
  if (!index(shown, to, producers, inBetween)) {
    return nullptr;
  }
  if (covered_.empty()) {
    lucent::debug(composeChannel, "record={} shown whole: no keyed object", shown.sequence());
    return nullptr;
  }
  const std::span<const RecordEntry> entries = shown.entries();
  const std::span<const SlotStart> starts = shown.slotStarts();
  heads_.clear();
  tableStarts_.clear();
  for (std::size_t i = 0; i < starts.size(); i++) {
    heads_.emplace_back(slotKey(starts[i].slot), i);
    tableStarts_.push_back(i);
  }
  std::sort(heads_.begin(), heads_.end());
  std::sort(tableStarts_.begin(), tableStarts_.end(), [&starts](std::size_t a, std::size_t b) {
    return std::tie(starts[a].slot.table, a) < std::tie(starts[b].slot.table, b);
  });
  // The draw state in effect at each position: that of the first primitive at or after it, else the last one.
  environments_.assign(entries.size() + 1, nullptr);
  const RecordDrawState *next = nullptr;
  for (std::size_t i = entries.size(); i-- > 0;) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entries[i])) {
      next = &primitive->state;
    }
    environments_[i] = next;
  }
  for (std::size_t i = 1; i < environments_.size(); i++) {
    if (environments_[i] == nullptr) {
      environments_[i] = environments_[i - 1];
    }
  }
  outputPooled_ = false;
  removed_.assign(entries.size(), false);
  inserted_.clear();
  bool replaced = false;
  for (const Covered &object : covered_) {
    std::optional<std::span<const std::byte>> earlier = from ? from->find(object.id) : std::nullopt;
    // Only an object the previous frame skipped appears from rest; one it drew ambiguously has no state.
    const bool born = inBetween && !from->holds(object.id);
    if (!earlier && born && object.render->rests(object.to, resting(object.to.size()))) {
      earlier = std::span<const std::byte>(resting_);
    }
    // An object S' skipped with no state to appear from is born at S: absent, as in S', until t = 1.
    if (!earlier && born) {
      replaced = true;
      for (std::size_t at = object.begin; at < object.end; ++at) {
        removed_[keyed_[at].entry] = true;
      }
      continue;
    }
    // An object S' drew with no single state would draw S's moment inside an in-between: the interval is held.
    if (!earlier && inBetween) {
      lucent::debug(composeChannel,
                    "record={} in-between held: object {:#010x}/{:#010x} has no state in S'",
                    shown.sequence(),
                    object.id.producer,
                    object.id.object);
      refusal_ = {HeldCause::Unpaired, object.id.producer, false, object.firstEntry};
      return nullptr;
    }
    const StateInterval states{.from = earlier.value_or(object.to), .to = object.to, .t = t};
    const auto run = std::span<const Keyed>(keyed_).subspan(object.begin, object.end - object.begin);
    const Placement placement = place(shown, object.id, *object.render, states, run, vram, inBetween);
    if (placement == Placement::Refused) {
      return nullptr;
    }
    if (placement == Placement::Kept) {
      continue;
    }
    replaced = true;
    for (const Keyed &keyed : run) {
      removed_[keyed.entry] = true;
    }
    for (auto &[position, primitive] : placed_) {
      inserted_.push_back({position, inserted_.size(), primitive});
    }
  }
  // An object the previous frame drew and this one skipped, where the skip is a state, moves toward it.
  if (inBetween) {
    from->forEachObject([&](ObjectId id, std::span<const std::byte> state) {
      const StateProducer *render = producers.find(id.producer);
      if (render == nullptr || to.holds(id) || !render->rests(state, resting(state.size()))) {
        return;
      }
      const StateInterval states{.from = state, .to = resting_, .t = t};
      if (place(shown, id, *render, states, {}, vram, false) == Placement::Drawn) {
        replaced = true;
        for (auto &[position, primitive] : placed_) {
          inserted_.push_back({position, inserted_.size(), primitive});
        }
      }
    });
  }
  if (!replaced) {
    return nullptr;
  }

  std::sort(inserted_.begin(), inserted_.end(), [](const Insert &a, const Insert &b) {
    return std::tie(a.position, a.order) < std::tie(b.position, b.order);
  });
  entries_.clear();
  auto insert = inserted_.begin();
  for (std::size_t i = 0; i <= entries.size(); i++) {
    for (; insert != inserted_.end() && insert->position == i; ++insert) {
      entries_.emplace_back(insert->primitive);
    }
    if (i < entries.size() && !removed_[i]) {
      entries_.push_back(entries[i]);
    }
  }
  FrameRecord &composed = output(shown);
  composed.replaceEntries(entries_);
  return &composed;
}

} // namespace psx::present
