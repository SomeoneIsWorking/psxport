// Attribution for a render queue: who filled it, and with how much genuinely distinct geometry.
//
// push() fail-fasts at RQ_MAX rather than dropping prims, and until 2026-09-19 its message guessed at
// the cause in a parenthesis — "(runaway re-submission?)" — with no counts at all. That guess cannot be
// checked, and the two causes it conflates need opposite fixes:
//
//   * a RUNAWAY: one submit path re-submitting geometry it already submitted this frame. The queue is
//     full of repeats. Raising RQ_MAX would only postpone the abort.
//   * a CAPACITY shortfall: a frame that legitimately contains more distinct geometry than RQ_MAX
//     holds. Nothing is repeated. Here the capacity, and the measurement behind it, is what is wrong.
//
// This owner answers which one, from the queue itself, so the fatal reports a measurement instead of a
// hypothesis. It is deliberately free of Core, Game and GpuState: it reads a span of RqItem and nothing
// else, so the suite can drive it on constructed queues of both shapes.
#pragma once

#include "render_queue.h"
#include <cstdint>
#include <string>
#include <vector>

namespace psxport::render {

// One entity node's share of the queue. `node` is RqItem::dbg_node (0 = un-owned).
//
// READ THIS BEFORE BLAMING A NODE. dbg_node is the overlay's DISPLAY identity, and for deferred-flush
// world geometry it is weak: geometry submission is decoupled from the per-object entity walk, so a
// whole field's worth of faces can arrive under one node value that names the last object the walk
// touched rather than their true owner (Tomba! 2, game/render/submit.cpp). A dominant node here says
// "one submission phase produced these", not "this entity owns these".
struct QueueNodeShare {
  std::uint32_t node = 0;
  int prims = 0;
  int distinct = 0;
};

class RenderQueueAttribution {
public:
  // Reads `count` items. `count` may be any size; the report describes exactly what it was given, and
  // says so, because a report that cannot state its own denominator is not evidence.
  RenderQueueAttribution(const RqItem *items, int count);

  int prims() const {
    return mPrims;
  }
  // Prims whose geometry no earlier prim in the same queue already carried.
  int distinct() const {
    return mDistinct;
  }
  // Prims repeating geometry an earlier prim already submitted this frame.
  int repeats() const {
    return mPrims - mDistinct;
  }
  int nodes() const {
    return mNodes;
  }
  int primsInLayer(int layer) const;
  // Descending by prim count, largest first. At most kTopNodes entries.
  const std::vector<QueueNodeShare> &topNodes() const {
    return mTopNodes;
  }

  // The discriminator. True when repeats outnumber distinct geometry: a frame whose content genuinely
  // grew submits new prims, so a majority of exact repeats cannot be explained by more content. Stated
  // as a majority rather than a tuned ratio because the two populations are meant to be far apart —
  // a real runaway repeats a whole scene many times over, and a real capacity shortfall repeats almost
  // nothing. A queue between the two is reported with its counts and no verdict either way.
  bool looksLikeRunaway() const {
    return repeats() > mDistinct;
  }

  // The complete report, one line per fact, for the fatal to emit. Includes the denominator and the
  // per-layer split even when they are unremarkable: an overflow is read once, after the fact, from a
  // log, and a fact omitted for being uninteresting cannot be recovered.
  std::string text() const;

  static constexpr int kTopNodes = 8;

private:
  static std::uint64_t geometryKey(const RqItem &item);

  int mPrims = 0;
  int mDistinct = 0;
  int mNodes = 0;
  int mByLayer[RQ_LAYER_COUNT] = {};
  std::vector<QueueNodeShare> mTopNodes;
};

// The complete overflow fatal: attribute the queue, print the report and a backtrace, abort. The whole
// response to a full queue lives here rather than inside push(), so the queue owns admission and this
// owner owns what a full queue means.
[[noreturn]] void abortOnFullRenderQueue(const RqItem *items, int count);

// The same attribution for a queue that did NOT overflow, on the interned "rqattr" log channel. This is
// what makes an overflow report READABLE: "64,792 prims from one node" means nothing without the same
// number for a frame that was fine, or for the same frame in the other aspect. Walking the queue is the
// one case the project's logging rule allows a guard around, so it costs nothing when the channel is off.
// Turn it on with the framework's channel variable: PSXPORT_DEBUG=rqattr.
void logRenderQueueAttribution(int frame, const RqItem *items, int count);

// Everything a flushed queue reports about itself: the `rqflush` shape line and, separately, the
// `rqattr` attribution. Called once per real flush; each half costs nothing when its channel is off.
void logFlushedRenderQueue(int frame, const RqItem *items, int count, std::uint32_t seq);

} // namespace psxport::render
