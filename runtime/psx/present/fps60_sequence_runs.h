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
// The painter object is therefore part of the run key, not decoration on the line. The entity node
// is part of it for the same reason one level down: measured 2026-09-19 on Tomba! 2's hut interior,
// every world prim in the frame shares painter object 0, so the whole reconstructed world came out
// as ONE run of 643 items whose extent covered the picture. A run that spans the entire world names
// no more than a run that spans the entire layer. Runs are also what the dump iterates, so
// extracting them makes the grouping testable without a window, a guest program, or a frame.
//
// PURE. It reads the captured items and an ownership predicate. No Core, no queue, no logging.
#pragma once

#include "painter_object_layer.h"
#include "render_queue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace psxport::fps60 {

// The rectangle a span of items covers, in VRAM pixel coordinates — the space the rasterizer draws
// in (tritex.vert: `i_pos` is "VRAM pixel coords (post draw-offset)"). It is NOT the dump's own
// pixel coordinates: `gpu_vk_shot` writes the display region [s_last_sx, s_last_sy) sized
// s_last_w x s_last_h, so a reader joining an extent to dumped pixels subtracts that origin, which
// the shot line reports. This comment used to claim the two spaces were the same; they coincide
// only for a title that displays at 0,0. Half-open: `x1`/`y1` are one past the rightmost/bottom-most
// vertex, so `width()` and `height()` are pixel counts and an empty extent is zero-sized rather than
// one pixel wide.
//
// WHY A RUN CARRIES ONE. `tools/fps60_check.py` reports the screen TILE that failed to interpolate;
// this dump reports the PRODUCER of every verbatim run. Until the run carried an extent the two
// could not be joined, so a reproducible stale tile named no owner and the answer had to be guessed
// from cropped pixels. Measured 2026-09-19: Tomba! 2's hut interior is stale at tile (112,160) in 12
// of 120 triples, and project-state recorded that no stale share in either dumped scene had been
// attributed to a layer.
struct ScreenExtent {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;

  bool empty() const {
    return x1 <= x0 || y1 <= y0;
  }
  int width() const {
    return empty() ? 0 : x1 - x0;
  }
  int height() const {
    return empty() ? 0 : y1 - y0;
  }
  // Whether the half-open pixel rectangle [x,x+w) x [y,y+h) meets this one. A tile query asks with
  // the tile's own size, so the answer is "this run drew something inside that tile", not "the run
  // starts there".
  bool intersects(int x, int y, int w, int h) const {
    return !empty() && x < x1 && x0 < x + w && y < y1 && y0 < y + h;
  }
};

// One maximal stretch of adjacent items that share a layer, an ownership answer, a painter object
// and an entity node. `begin` and `end` index the span that was grouped, so a caller can reach the
// items themselves without this owner copying any. `extent` is the union of those items' screen
// vertices, so a run answers both "whose prims are these" and "where on the screen were they".
struct SequenceRun {
  int layer = 0;
  bool owned = false;
  PainterObjectId painterObject = 0;
  std::uint32_t dbgNode = 0;
  std::size_t begin = 0;
  std::size_t end = 0;
  ScreenExtent extent{};

  std::size_t count() const {
    return end - begin;
  }
};

// Appends every run of `items`, in order, so `runs` back to back covers the whole span exactly once.
// `owned` answers whether the temporal scene source reconstructs that item; pass a predicate that
// always returns false when no source is active. The result is empty only for an empty input, which
// is what tells a reader that a frame captured nothing rather than that the grouping matched
// nothing.
//
// TAKES POINTERS because the span that matters is the one the presenter EMITS. Fps60::presentPass
// merges the reconstructed sink over the captured queue — every item the scene source owns is
// REPLACED before anything is rasterised — and that merged stream is a vector of pointers into two
// different queues. Grouping the captured queue instead described items that were never drawn:
// measured 2026-09-19 on Tomba! 2's outdoor replay, the captured queue reported 132 items whose
// boxes covered 39% of the picture while the picture was 100% painted and its world visibly
// interpolated, so the runs could not be joined to the pixels they were supposed to explain.
void groupSequenceRuns(std::span<const RqItem *const> items,
                       const std::function<bool(const RqItem &)> &owned,
                       std::vector<SequenceRun> &runs);

} // namespace psxport::fps60
