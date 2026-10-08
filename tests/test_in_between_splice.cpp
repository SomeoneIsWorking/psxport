// `spliceInBetween`: which captured slot each reconstructed primitive stands in for.
//
// The unit is pure - a captured span, a reconstruction span, an ownership predicate and an out
// vector - so every case here is a hand-built pair of streams rather than a guest frame. What it
// pins is the pair of facts the painter planner checks after the splice, because the splice is what
// has to make them hold: the stream is sorted by (layer, draw_seq) inside one flush epoch, and each
// substituted primitive carries the three coordinates of the slot it fills.
//
// The case that matters most is a source whose producers publish in a DIFFERENT ORDER from the
// captured queue, with a different count per producer. That is what Spyro 1's picker does on every
// in-between frame (measured: captured owned slots A x196 B x167 C x195 D x692 E x404 against
// reconstruction C x197 D x681 A x199 B x167 E x401), and pairing by stream position instead of by
// producer is what aborted the product with refused=8 UnsortedQueue, refused=12 DuplicateReplayKey
// and refused=2 NonWorld on four different attempts.
#include "in_between_splice.h"
#include "testutil.h"

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace {

constexpr PainterObjectId kActor = 0x8001F798u;
constexpr PainterObjectId kSecondary = 0x800258F0u;
constexpr PainterObjectId kUnowned = 0xBEEFu; // a producer this source does not own
constexpr PainterObjectId kOverlay = 0u;      // ungrouped: a 2D primitive cannot carry a painter object
constexpr PainterReplayDomainId kDomain = 0x80022A00u;

RqItem item(int layer, uint32_t streamSeq, PainterObjectId producer) {
  RqItem result{};
  result.layer = (uint8_t)layer;
  result.seq = streamSeq;
  result.draw_seq = streamSeq;
  result.flush_ordinal = 0;
  result.sort_key = -1;
  result.painter_object = producer;
  result.nv = 3;
  if (producer != 0) {
    // A distinct authored replay position per item, so a duplicate in the spliced stream shows up
    // here and not only in the planner.
    result.painter_replay = {kDomain, {(uint16_t)streamSeq, producer, streamSeq}};
  }
  return result;
}

const std::function<bool(const RqItem &)> &claimTwoProducers() {
  static const std::function<bool(const RqItem &)> claim = [](const RqItem &primitive) {
    return primitive.painter_object == kActor || primitive.painter_object == kSecondary;
  };
  return claim;
}

const std::function<bool(const RqItem &)> &claimNothing() {
  static const std::function<bool(const RqItem &)> claim = [](const RqItem &) {
    return false;
  };
  return claim;
}

// The two invariants the painter planner refuses a stream without. Checked on every case here,
// because a splice that produces an unsortable stream aborts the product a few frames later, in a
// title, on a route nobody was testing.
void checkPlannerInvariants(const std::vector<const RqItem *> &stream) {
  for (size_t i = 0; i < stream.size(); ++i) {
    CHECK(stream[i] != nullptr);
    CHECK_EQ(stream[i]->flush_ordinal, stream.front()->flush_ordinal);
    if (i == 0) {
      continue;
    }
    const RqItem &previous = *stream[i - 1];
    const RqItem &current = *stream[i];
    CHECK(previous.layer <= current.layer);
    if (previous.layer == current.layer) {
      CHECK(previous.draw_seq <= current.draw_seq);
    }
  }
}

// A producer publishes its own reconstruction order; the captured queue holds that producer's slots
// in the captured order. Each side gets a distinct item count so a surplus and a shortfall are both
// reachable, and the captured queue ends with a layer-3 slot so the LAYER coordinate is exercised
// too - the 2D overlay is exactly the primitive whose layer a positional splice used to get wrong.
void test_reconstruction_is_paired_per_producer_not_per_stream_position() {
  const std::array<RqItem, 9> captured = {
      item(RQ_WORLD, 10, kActor),
      item(RQ_WORLD, 11, kUnowned),
      item(RQ_WORLD, 12, kSecondary),
      item(RQ_WORLD, 13, kUnowned),
      item(RQ_WORLD, 14, kActor),
      item(RQ_WORLD, 15, kSecondary),
      item(RQ_WORLD, 16, kSecondary),
      item(RQ_WORLD, 17, kUnowned),
      item(RQ_HUD, 18, kOverlay),
  };
  // The reconstruction publishes SECONDARY first and the actor second - the reverse of the captured
  // order - and gives secondary three primitives against the three slots it held.
  std::array<RqItem, 5> reconstruction = {
      item(RQ_WORLD, 0, kSecondary),
      item(RQ_WORLD, 1, kSecondary),
      item(RQ_WORLD, 2, kSecondary),
      item(RQ_WORLD, 3, kActor),
      item(RQ_WORLD, 4, kActor),
  };
  // What each emitted stream position must be, as an index into `captured` (unchanged) or
  // `reconstruction` (substituted). Measured against the product, not derived: pairing by producer
  // is what puts secondary's first primitive in secondary's first captured slot.
  const std::array<int, 9> expectedFromCaptured{-1, 1, -1, 3, -1, -1, -1, 7, 8};
  const std::array<int, 9> expectedFromReconstruction{3, -1, 0, -1, 4, 1, 2, -1, -1};

  std::vector<const RqItem *> stream;
  const psxport::fps60::SpliceCensus census =
      psxport::fps60::spliceInBetween(captured, reconstruction, stream, claimTwoProducers());

  CHECK_EQ(census.ownedSlots, 5);
  CHECK_EQ(census.substituted, 5);
  CHECK_EQ(census.declined, 0);
  CHECK_EQ(census.surplus, 0);
  CHECK_EQ(stream.size(), captured.size());
  checkPlannerInvariants(stream);
  for (size_t i = 0; i < captured.size(); ++i) {
    if (expectedFromReconstruction[i] >= 0) {
      CHECK(stream[i] == &reconstruction[(size_t)expectedFromReconstruction[i]]);
      // A substituted primitive must carry the identity of the SLOT's producer, not of its own.
      CHECK_EQ(stream[i]->painter_object, captured[i].painter_object);
    } else {
      CHECK(stream[i] == &captured[(size_t)expectedFromCaptured[i]]);
    }
  }
}

// A substituted primitive replaces the GEOMETRY, not the position in the stream: it adopts all three
// of the coordinates of the slot it fills. Its own vertex data is untouched, which is what makes it
// the reconstruction's geometry and not a copy of the captured one.
void test_a_substituted_primitive_adopts_all_three_coordinates_of_its_slot() {
  std::array<RqItem, 2> captured = {
      item(RQ_WORLD, 40, kActor),
      item(RQ_WORLD, 41, kSecondary),
  };
  captured[0].flush_ordinal = 7;
  captured[1].flush_ordinal = 7;
  std::array<RqItem, 2> reconstruction = {
      item(RQ_WORLD, 0, kActor),
      item(RQ_WORLD, 1, kSecondary),
  };
  reconstruction[0].flush_ordinal = 0; // push() stamps 0; the captured queue's epoch is 7
  reconstruction[0].layer = RQ_HUD;    // and a layer from the reconstruction's own queue
  reconstruction[0].draw_seq = 999;
  reconstruction[0].xsf[0] = 12.5f; // the geometry the source actually produced
  captured[0].xsf[0] = -1.0f;
  std::vector<const RqItem *> stream;
  const auto census = psxport::fps60::spliceInBetween(captured, reconstruction, stream, claimTwoProducers());
  CHECK_EQ(census.substituted, 2);
  CHECK_EQ(stream.size(), 2u);
  CHECK(stream[0] == &reconstruction[0]);
  CHECK_EQ(stream[0]->layer, captured[0].layer);
  CHECK_EQ(stream[0]->draw_seq, captured[0].draw_seq);
  CHECK_EQ(stream[0]->flush_ordinal, captured[0].flush_ordinal);
  CHECK_EQ(stream[0]->xsf[0], 12.5f);
  checkPlannerInvariants(stream);
}

// A source REBUILDS, so its item count is its own: it can outrun the slots its own producers held,
// and a producer can fall short of them. The surplus is DROPPED and counted; the shortfall drops the
// owned slots with nothing to put in them. Neither moves an un-owned captured item, and neither is
// silent - `census` is the number the presenter reports.
void test_per_producer_surplus_and_shortfall_are_counted_and_never_move_an_unowned_item() {
  const std::array<RqItem, 7> captured = {
      item(RQ_WORLD, 20, kActor),
      item(RQ_WORLD, 21, kUnowned),
      item(RQ_WORLD, 22, kSecondary),
      item(RQ_WORLD, 23, kActor),
      item(RQ_WORLD, 24, kUnowned),
      item(RQ_WORLD, 25, kSecondary),
      item(RQ_WORLD, 26, kSecondary),
  };
  // The actor falls SHORT (one primitive against its two captured slots) and secondary runs one
  // LONG against its three.
  std::array<RqItem, 5> reconstruction = {
      item(RQ_WORLD, 0, kActor),
      item(RQ_WORLD, 1, kSecondary),
      item(RQ_WORLD, 2, kSecondary),
      item(RQ_WORLD, 3, kSecondary),
      item(RQ_WORLD, 4, kSecondary),
  };
  std::vector<const RqItem *> stream;
  const auto census = psxport::fps60::spliceInBetween(captured, reconstruction, stream, claimTwoProducers());
  CHECK_EQ(census.ownedSlots, 5);
  CHECK_EQ(census.substituted, 4);
  CHECK_EQ(census.declined, 1);
  CHECK_EQ(census.surplus, 1);
  // The second actor slot is dropped and the surplus secondary primitive is dropped; the two
  // un-owned items are still there, in their captured order, and the stream is still sortable.
  CHECK_EQ(stream.size(), captured.size() - 1);
  CHECK(stream[0] == &reconstruction[0]);
  CHECK(stream[1] == &captured[1]);
  CHECK(stream[2] == &reconstruction[1]);
  CHECK(stream[3] == &captured[4]);
  CHECK(stream[4] == &reconstruction[2]);
  CHECK(stream[5] == &reconstruction[3]);
  checkPlannerInvariants(stream);
}

// A source that owns NOTHING in the captured queue has no captured positions at all, so its whole
// reconstruction is the present's stream. This is the `world_temporal` shape: geometry built from the
// previous and current endpoints rather than from this frame, on a frame that captured nothing.
void test_a_source_that_owns_nothing_emits_its_whole_reconstruction() {
  const std::array<RqItem, 2> captured = {
      item(RQ_WORLD, 30, kUnowned),
      item(RQ_HUD, 31, kOverlay),
  };
  std::array<RqItem, 3> reconstruction = {
      item(RQ_WORLD, 0, kActor),
      item(RQ_WORLD, 1, kActor),
      item(RQ_WORLD, 2, kActor),
  };
  std::vector<const RqItem *> stream;
  const auto census = psxport::fps60::spliceInBetween(captured, reconstruction, stream, claimNothing());
  CHECK_EQ(census.ownedSlots, 0);
  CHECK_EQ(census.substituted, 3);
  CHECK_EQ(census.surplus, 0);
  CHECK_EQ(census.declined, 0);
  CHECK_EQ(stream.size(), captured.size() + reconstruction.size());
  for (size_t i = 0; i < captured.size(); ++i) {
    CHECK(stream[i] == &captured[i]);
  }
  for (size_t i = 0; i < reconstruction.size(); ++i) {
    CHECK(stream[captured.size() + i] == &reconstruction[i]);
  }
}

} // namespace

int main() {
  RUN(reconstruction_is_paired_per_producer_not_per_stream_position);
  RUN(a_substituted_primitive_adopts_all_three_coordinates_of_its_slot);
  RUN(per_producer_surplus_and_shortfall_are_counted_and_never_move_an_unowned_item);
  RUN(a_source_that_owns_nothing_emits_its_whole_reconstruction);
  return pt_summary();
}
