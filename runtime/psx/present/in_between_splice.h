// Substituting one present's reconstruction for the primitives its source owns, in place.
//
// WHY IT IS ITS OWN OWNER. `Fps60::presentPass` used to build the in-between stream itself, and
// every attempt to make that stream satisfy the painter planner was a change to the ORDERING RULES
// instead of to the question the rules actually ask. The rule is "which captured item does this
// reconstructed primitive stand in for", and the answer is per PRODUCER, not per stream position.
//
// MEASURED, on Spyro 1's picker at `fps60=1` (the shipping default), on one aborting frame:
//
//   captured owned slots, in captured order   A x196  B x167  C x195  D x692  E x404  (overlay) x1
//   reconstruction items, in its own order    C x197  D x681  A x199  B x167  E x401  (overlay) x1
//
// The two streams are the SAME producers' output in a DIFFERENT ORDER, and per producer their
// COUNTS differ too (A 196/199, D 692/681, E 404/401). So "the i-th reconstructed item replaces the
// i-th owned slot" is not a mapping, it is a coincidence that happens to hold for one frame. What it
// did was put producer C's faces where producer A's faces stood, which is how a single in-between
// frame aborted the product four different ways:
//
//   refused=8  UnsortedQueue      - the overlay's layer-3 quad was placed in a layer-1 world slot,
//                                   and the next captured world primitive then had a lower layer
//                                   than its predecessor. Measured: `ORDER i=1675 prev=sink#1645
//                                   layer=3 draw_seq=1674 | cur=cap#1683 layer=1 draw_seq=1683`.
//   refused=12 DuplicateReplayKey - the same substitution asked one producer's replay keys to
//                                   occupy another's slots.
//   refused=2  NonWorld           - the painter face that landed in the overlay's slot.
//
// The fix is not a new ordering rule: it is that a reconstructed primitive takes the slot of ITS OWN
// PRODUCER. Nothing else about the splice changes, and the two facts the planner checks afterwards
// become consequences again instead of accidents: the stream's (layer, draw_seq) sequence is the
// captured queue's, and each substituted item adopts all three of the coordinates of the slot it
// fills.
//
// THE ORDERING CONTRACT THE SPLICE EXISTS TO KEEP is untouched by any of this: an un-owned captured
// item is emitted at its own index and never moves relative to another un-owned captured item. That
// is the whole reason the in-between stopped flickering - measured on Spyro 1, the two presents of
// ONE logic frame carry byte-identical item lists and still differed on 11.7-27.7% of the pixels in
// the flame's own region while a control region read exactly 0.00%, because a translucent item had
// composited against a different background on the in-between pass.
//
// PURE. It reads the captured items and an ownership predicate and writes pointers into the
// reconstruction. No Core, no queue, no logging, no title knowledge: the producer identity it pairs
// on is `RqItem::painter_object`, which the queue already carries on every grouped face.
#pragma once

#include "painter_object_layer.h"
#include "render_queue.h"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace psxport::fps60 {

// WHAT ONE SPLICE DID. The presenter reports `surplus` and `declined`; both are numbers of
// primitives a source produced or a captured primitive lost, so neither may be silent.
struct SpliceCensus {
  // Captured items the source claimed.
  int ownedSlots = 0;
  // Owned slots that received reconstruction geometry.
  int substituted = 0;
  // Owned slots with no reconstruction primitive left to place: the source declined to reproduce
  // that primitive this present, and presenting the captured copy would defeat the replacement.
  int declined = 0;
  // Reconstructed primitives with no captured slot of their own producer to fill.
  int surplus = 0;
};

// Replaces every captured item `owns` claims with the next reconstruction primitive of the SAME
// producer, and appends `out` in the captured queue's order. Un-owned items are appended by pointer,
// so every one of them keeps its own index.
//
// `reconstruction` is mutable because a substituted primitive ADOPTS the three stream coordinates of
// the slot it fills - `layer`, `draw_seq` and `flush_ordinal` - which is what puts the reconstruction
// inside the captured run in the captured order. All three are stamped by `push()`, so a
// reconstruction's own values come from a queue that never existed alongside the captured one, and
// the painter planner refuses a run whose `flush_ordinal`s differ (MixedFlushEpoch) or which is not
// sorted by (layer, draw_seq) (UnsortedQueue).
//
// A source that owns NOTHING in the captured queue emits its whole reconstruction after it, in its
// own order: it has no captured positions at all, so its geometry is all there is to draw.
SpliceCensus spliceInBetween(std::span<const RqItem> captured,
                             std::span<RqItem> reconstruction,
                             std::vector<const RqItem *> &out,
                             const std::function<bool(const RqItem &)> &owns);

} // namespace psxport::fps60
