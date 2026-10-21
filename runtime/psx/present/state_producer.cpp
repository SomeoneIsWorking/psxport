// state_producer.cpp — a title's table of renders, and the list a render emits into.
#include "state_producer.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace psx::present {
namespace {

std::uint64_t placeKey(std::uint32_t packet, int vertex) {
  return (static_cast<std::uint64_t>(packet) << 2) | static_cast<std::uint64_t>(vertex);
}

// A place lies within one rounding of its whole pixel each way; farther, the GPU wrapped the 11-bit word.
bool placesWhole(int whole, float place) {
  return std::abs(place - static_cast<float>(whole)) < 2.0f;
}

} // namespace

void PrimitiveList::place(std::uint32_t packet, int vertex, const GtePlace &place) {
  const std::uint64_t key = placeKey(packet, vertex);
  placesSorted_ = placesSorted_ && (places_.empty() || places_.back().key < key);
  places_.push_back({key, place});
}

void PrimitiveList::clear() {
  primitives.clear();
  places_.clear();
  placesSorted_ = true;
}

std::optional<GtePlace> PrimitiveList::placed(std::uint32_t packet, int vertex) {
  if (places_.empty()) {
    return std::nullopt;
  }
  if (!placesSorted_) {
    std::ranges::stable_sort(places_, {}, &VertexPlace::key);
    placesSorted_ = true;
  }
  const std::uint64_t key = placeKey(packet, vertex);
  const auto found = std::ranges::upper_bound(places_, key, {}, &VertexPlace::key);
  if (found == places_.begin() || std::prev(found)->key != key) {
    return std::nullopt;
  }
  return std::prev(found)->place;
}

void PrimitiveList::emit(OtSlot slot, const DrawPrimitive &primitive) {
  DrawPrimitive &kept = primitives.emplace_back(slot, primitive).second;
  if (kept.kind == PrimitiveKind::Sprite || kept.sourceAddress == 0) {
    return;
  }
  for (int i = 0; i < kept.vertexCount; ++i) {
    const std::optional<GtePlace> place = placed(kept.sourceAddress, i);
    if (!place) {
      continue;
    }
    RecordVertex &vertex = kept.vertices[static_cast<std::size_t>(i)];
    if (!placesWhole(vertex.x, place->x) || !placesWhole(vertex.y, place->y)) {
      continue;
    }
    vertex.subX = place->x - static_cast<float>(vertex.x);
    vertex.subY = place->y - static_cast<float>(vertex.y);
  }
}

void StateProducerTable::install(std::uint32_t producer, std::unique_ptr<const StateProducer> render) {
  if (!render || !renders_.try_emplace(producer, std::move(render)).second) {
    lucent::error("present", "producer 0x{:08X} installed twice or without a render", producer);
    std::abort();
  }
}

const StateProducer *StateProducerTable::find(std::uint32_t producer) const {
  const auto found = renders_.find(producer);
  return found == renders_.end() ? nullptr : found->second.get();
}

} // namespace psx::present
