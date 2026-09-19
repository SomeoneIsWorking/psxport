// Grouping a captured frame's queue into the runs the fps60 presenter treats alike.
//
// WHY IT IS ITS OWN OWNER. The presenter's diagnostic dump used to group a captured frame by layer
// and by whether the temporal scene source owns the item, and print one line per run. That could
// say HOW MUCH of a frame replayed verbatim and in which layer, but never WHOSE it was: several
// producers draw into RQ_WORLD, so one "verbatim n=478" line spanned all of them and named none.
// Measured 2026-09-19 on Spyro's 7,200-field Artisans replay, 1,613,113 of 3,768,952 captured items
// replayed verbatim while every producer the project had left on its list drew about 32 faces per
// logic frame between them — so the number the dump could not attribute was the whole question.
//
// The painter object is therefore part of the run key, not decoration on the line. Runs are also
// what the dump iterates, so extracting them makes the grouping testable without a window, a guest
// program, or a frame.
//
// PURE. It reads the captured items and an ownership predicate. No Core, no queue, no logging.
#pragma once

#include "painter_object_layer.h"
#include "render_queue.h"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace psxport::fps60 {

// One maximal stretch of adjacent captured items that share a layer, an ownership answer and a
// painter object. `begin` and `end` index the captured frame, so a caller can reach the items
// themselves without this owner copying any.
struct SequenceRun {
  int layer = 0;
  bool owned = false;
  PainterObjectId painterObject = 0;
  std::size_t begin = 0;
  std::size_t end = 0;

  std::size_t count() const {
    return end - begin;
  }
};

// Appends every run of `items`, in order, so `runs` back to back covers the whole span exactly once.
// `owned` answers whether the temporal scene source reconstructs that item; pass a predicate that
// always returns false when no source is active. The result is empty only for an empty input, which
// is what tells a reader that a frame captured nothing rather than that the grouping matched
// nothing.
void groupSequenceRuns(std::span<const RqItem> items,
                       const std::function<bool(const RqItem &)> &owned,
                       std::vector<SequenceRun> &runs);

} // namespace psxport::fps60
