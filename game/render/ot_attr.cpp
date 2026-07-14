// OtAttr implementation — see ot_attr.h for the design. Separated from the header so the header can
// stay a forward-declared-Core, dependency-light include (same split as pkt_span.h/.cpp).
#include "ot_attr.h"
#include "render.h"
#include "render_internal.h"   // cur_render_node — same node fallback the native submit path itself uses
#include "core.h"
#include "game.h"
#include "cfg.h"

// Packet pool range — SAME bytes PktSpan watches (game/render/pkt_span.h): the shared render-buffer
// map every GP0 packet submitter (owned native or still-substrate) writes into.
static constexpr uint32_t POOL_LO = 0x800BFE68u;
static constexpr uint32_t POOL_HI = 0x800E7E68u;

void OtAttr::resetIfNewFrame(uint32_t frame) {
  if (frame == mFrame) return;
  mFrame = frame;
  mSpanCount = 0;
  mSpanOverflow = 0;
}

void OtAttr::trackStore(Core* c, uint32_t addr, uint32_t bytes) {
  if (!cfg_dbg("otattr")) return;   // single predicted-false check gates everything below when off
  const uint32_t k = addr | 0x80000000u;
  if (k < POOL_LO || k >= POOL_HI) return;

  resetIfNewFrame(c->game->gpu.s_frame);

  const uint32_t fn     = c->idiag.otattrTop();
  const uint32_t caller = c->idiag.otattrCaller();
  // Same node fallback the native GT3/GT4 submit path itself uses (render_internal.h cur_render_node):
  // the walk's beginObject() node when set, else the guest "current render object" scratchpad
  // (0x1F80028C) — most native per-object quad submission (submit.cpp) never opens a diag walk scope,
  // it relies on this scratchpad, so reading raw diag.currentNode() alone would show node=0 for the
  // MAJORITY of world-object quads (the exact case bug #45 cares about).
  const uint32_t node   = cur_render_node(c);

  // Coalesce a run of stores sharing the same attribution into one growing span (mirrors PktSpan's own
  // span-merge idea) — a quad's header/vertex/color words collapse to a single entry instead of one
  // per store, which is what keeps SPAN_CAP from blowing out on a normal frame.
  if (mSpanCount > 0) {
    Span& last = mSpans[mSpanCount - 1];
    if (last.fn == fn && last.caller == caller && last.node == node && k >= last.lo && k <= last.hi) {
      if (k + bytes > last.hi) last.hi = k + bytes;
      return;
    }
  }
  if (mSpanCount < SPAN_CAP) mSpans[mSpanCount++] = Span{ k, k + bytes, fn, caller, node };
  else mSpanOverflow++;
}

void OtAttr::trackGte(Core* c) {
  if (!cfg_dbg("otattr")) return;
  const uint32_t frame = c->game->gpu.s_frame;
  if (frame != mGteFrame) { mGteFrame = frame; mGteCount = 0; mGteOverflow = 0; }

  const uint32_t fn   = c->idiag.otattrTop();
  const uint32_t node = cur_render_node(c);
  for (int i = 0; i < mGteCount; i++)
    if (mGte[i].fn == fn && mGte[i].node == node) { mGte[i].count++; return; }
  if (mGteCount < GTE_CAP) mGte[mGteCount++] = GteBucket{ fn, node, 1 };
  else mGteOverflow++;
}

bool OtAttr::lookupStore(uint32_t addr, Span* out) const {
  const uint32_t k = addr | 0x80000000u;
  // Most-recent-first: within one frame the pool pointer is monotonic, so addresses aren't reused, but
  // scanning backward costs nothing extra and is the more useful order if that ever changes.
  for (int i = mSpanCount - 1; i >= 0; i--)
    if (k >= mSpans[i].lo && k < mSpans[i].hi) { if (out) *out = mSpans[i]; return true; }
  return false;
}
