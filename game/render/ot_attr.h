// class OtAttr — OT/GTE SUBMISSION ATTRIBUTION tool (`debug otattr`).
//
// MOTIVATION (USER 2026-07-14, serves bug #45 + general render investigations): trace every guest-
// submitted primitive (packet-pool store) and every GTE RTPS/RTPT execution back to (a) the guest
// FUNCTION that submitted it and (b) the world-object NODE whose render-walk scope was open at the
// time. "Which fn+node drew this dialog-panel packet / this gem quad" without hand-walking backwards
// through addresses.
//
// DIAGNOSTIC ONLY. Zero behavior change when the `otattr` debug channel is off (a single cfg_dbg()
// check gates every method — same idiom as PktSpan's `!mArmed` early-out). Never writes guest memory;
// only reads Core::mem_r* / Render::diag / InterpDiag's otattr shadow stack.
//
// Two independent tables, both per-Core (SBS runs two: no cross-contamination) and both keyed to the
// CURRENT render frame (GpuState::s_frame) with a LAZY reset on first touch of a new frame — so the
// data for the just-finished frame is still readable any time before the NEXT frame's first store/GTE
// op, which is exactly when a REPL command run between `run N` steps executes. Sized statically;
// overflow is COUNTED and reported rather than growing unbounded or silently dropping.
//
//   1. Packet-pool store spans — every guest store landing in the shared packet pool
//      [0x800BFE68, 0x800E7E68) (same range PktSpan watches) is attributed to {emitter fn = the
//      OTATTR SHADOW STACK top (InterpDiag::otattrTop(), the innermost guest fn reached via an
//      INDIRECT/jalr rec_dispatch — see interp_diag.h), caller fn = one frame below that, node =
//      Render::diag.currentNode()}. Consecutive stores with the SAME (fn, node) attribution are
//      COALESCED into one growing [lo,hi) span (mirrors PktSpan's own span-merge idea) so one quad's
//      handful of header/vertex/color words collapse to a single entry instead of one-per-word —
//      this is what keeps the static cap (OT_ATTR_CAP) from blowing out on a normal frame.
//
//   2. GTE RTPS/RTPT per-(fn,node) call counts for the frame (not per-call — aggregated), fed from
//      gte_op's existing RTPS/RTPT branch (runtime/recomp/gte_beetle.cpp, alongside rtpcaller_record).
//
// Reached as `core->mRender->otAttr`. The REPL `otattr` command (repl.cpp) re-walks the CURRENT OT
// read-only (no gpu_gp0 side effects) and looks up each packet's pool address in table 1, printing the
// GTE histogram from table 2 alongside.
#pragma once
#include <stdint.h>
class Core;

class OtAttr {
public:
  // A single terrain-heavy field frame can produce well over 4096 attribution-distinct spans (terrain
  // tiles interleave E1/E2 texpage-state words between quads, which breaks the lo/hi coalescing even
  // when the fn/node attribution is identical) — sized generously since this is diagnostic-only memory,
  // live only while the `otattr` channel is enabled.
  static constexpr int SPAN_CAP = 65536;
  static constexpr int GTE_CAP  = 512;

  struct Span { uint32_t lo, hi; uint32_t fn, caller, node; };
  struct GteBucket { uint32_t fn, node, count; };

  // Called from Core::mem_w8/16/32 (mem.cpp) for EVERY guest store — no-op unless the `otattr` channel
  // is enabled (checked internally, same as PktSpan's !mArmed hot-path no-op) and the address falls in
  // the packet-pool range.
  void trackStore(Core* c, uint32_t addr, uint32_t bytes);

  // Called from gte_op's RTPS/RTPT branch (gte_beetle.cpp) — aggregates a call count per (fn, node).
  void trackGte(Core* c);

  // REPL/diagnostic readback — NOT hot path, may scan linearly.
  bool lookupStore(uint32_t addr, Span* out) const;
  int  spanCount()  const { return mSpanCount; }
  int  spanOverflow() const { return mSpanOverflow; }
  const Span* spanAt(int i) const { return &mSpans[i]; }
  int  gteCount() const { return mGteCount; }
  int  gteOverflow() const { return mGteOverflow; }
  const GteBucket* gteAt(int i) const { return &mGte[i]; }
  uint32_t frame() const { return mFrame; }

private:
  void resetIfNewFrame(uint32_t frame);

  uint32_t mFrame = 0xFFFFFFFFu;
  Span     mSpans[SPAN_CAP] = {};
  int      mSpanCount = 0;
  int      mSpanOverflow = 0;

  uint32_t mGteFrame = 0xFFFFFFFFu;
  GteBucket mGte[GTE_CAP] = {};
  int       mGteCount = 0;
  int       mGteOverflow = 0;
};
