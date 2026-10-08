#include "in_between_splice.h"

namespace psxport::fps60 {
namespace {

// ONE PRODUCER'S RECONSTRUCTION ITEMS, IN THE ORDER THE SOURCE EMITTED THEM.
//
// The identity is `RqItem::painter_object`, the queue's own producer identity: every grouped face
// carries the key its producer published under, and the framework's contract for that field is
// already "opaque to the framework; games allocate identities, while the renderer only groups equal
// non-zero values". Pairing on it therefore asks the renderer nothing new. An UNGROUPED primitive
// carries 0, so a source that owns 2D or line geometry is one group under 0, ordered by the
// captured queue like any other.
struct ProducerItems {
  PainterObjectId id = 0;
  std::vector<RqItem *> items;
};

} // namespace

SpliceCensus spliceInBetween(std::span<const RqItem> captured,
                             std::span<RqItem> reconstruction,
                             std::vector<const RqItem *> &out,
                             const std::function<bool(const RqItem &)> &owns) {
  SpliceCensus census;
  out.clear();
  out.reserve(captured.size() + reconstruction.size());

  // The reconstruction's own producer order, grouped. A handful of producers at most, so a linear
  // scan is cheaper than a hash and keeps the pairing order-deterministic.
  std::vector<ProducerItems> producers;
  for (RqItem &item : reconstruction) {
    ProducerItems *producer = nullptr;
    for (ProducerItems &candidate : producers) {
      if (candidate.id == item.painter_object) {
        producer = &candidate;
      }
    }
    if (!producer) {
      producers.push_back({item.painter_object, {}});
      producer = &producers.back();
    }
    producer->items.push_back(&item);
  }

  // One cursor per producer: each captured slot of producer P takes P's next reconstruction
  // primitive, so one producer's geometry can never land in another producer's run however the two
  // orders compare.
  std::vector<std::size_t> taken(producers.size(), 0);
  for (const RqItem &item : captured) {
    if (!owns(item)) {
      out.push_back(&item);
      continue;
    }
    ++census.ownedSlots;
    std::size_t which = producers.size();
    for (std::size_t p = 0; p < producers.size(); ++p) {
      if (producers[p].id == item.painter_object) {
        which = p;
      }
    }
    if (which == producers.size() || taken[which] >= producers[which].items.size()) {
      ++census.declined;
      continue;
    }
    RqItem &replacement = *producers[which].items[taken[which]++];
    // THE RECONSTRUCTION REPLACES THE GEOMETRY, NOT THE POSITION IN THE STREAM.
    replacement.layer = item.layer;
    replacement.draw_seq = item.draw_seq;
    replacement.flush_ordinal = item.flush_ordinal;
    out.push_back(&replacement);
    ++census.substituted;
  }

  if (census.ownedSlots == 0) {
    // THE SOURCE OWNS NOTHING IN THE CAPTURED QUEUE, so the reconstruction IS this present's stream.
    // A source may legitimately emit geometry the guest's captured queue does not carry - its items
    // are built from the previous and current endpoints, not from this frame - and this frame can
    // capture nothing at all. There is no captured run structure to respect, so the whole
    // reconstruction is emitted, which is what the original (layer, seq) merge did in this case.
    for (RqItem &item : reconstruction) {
      out.push_back(&item);
    }
    census.substituted = static_cast<int>(reconstruction.size());
    return census;
  }

  census.surplus = static_cast<int>(reconstruction.size()) - census.substituted;
  return census;
}

} // namespace psxport::fps60
