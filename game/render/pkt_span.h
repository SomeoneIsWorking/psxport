// class PktSpan — packet-pool STORE-ADDRESS-SPAN tracker on one Core.
//
// Tomba's PSX render pipeline flushes objects' GP0 packets into a shared packet pool in guest RAM,
// [0x800BFE68, 0x800E7E68), that the OT walk later drains in ORDER. Some unowned overlay renderers
// write into this pool WITHOUT advancing the pool pointer (0x800BF544) — so the pointer can't bound
// their span. But `Core::mem_w8/16/32` sees every store, so we watch it: while a session is armed
// (open()), each store to the pool range extends this session's [lo,hi). The caller closes the session
// with close(&lo, &hi) and tags that address span with the object's PC-native world-position depth so
// its 2D billboard prims occlude for real at the deferred OT walk (fixes rope/flame drew-over-terrain).
//
// NESTING is required: some renderers dispatch the universal render command (ov_render_cmd) for the
// SAME object, and per-frame span attribution (ffspan) brackets sibling phases at the top level. Two
// nesting primitives:
//
//   1. RAII object scope — PktSpanSession sess(core); ... ; sess.close(&lo, &hi);
//      Saves the outer session's state on open, opens a fresh empty session, and on close restores +
//      MERGES this session's [lo,hi) into the outer so the outer's final depth-tag covers ALL packets.
//
//   2. Manual outer/inner snapshot — for the extern "C" ffspan_* attribution API in engine_stage.cpp
//      that has to look like C (called from native_boot's native_step_frame without a C++ context on
//      each invocation). save() returns an opaque snapshot; open() clears + arms; restoreMerge(snap)
//      restores the outer and merges the just-closed inner's span into it.
//
// Per-Core state so SBS / dualcore run two cores without cross-contamination (was the process-globals
// g_pkt_track / g_pkt_lo / g_pkt_hi; deglobalize-game 2026-07-02). Reached as `core->mRender->pktSpan`.
#pragma once
#include <stdint.h>
class Core;

class PktSpan {
public:
  // Called from Core::mem_w8/16/32 for every guest store. No-op unless armed. Inlined for the hot path.
  void track(uint32_t addr, uint32_t bytes) {
    if (!mArmed) return;
    uint32_t k = addr | 0x80000000u;
    if (k >= 0x800BFE68u && k < 0x800E7E68u) {
      if (k < mLo) mLo = k;
      if (k + bytes > mHi) mHi = k + bytes;
    }
  }

  // Manual outer/inner nesting primitive (used by ffspan_begin/end + the FFS macro in engine_stage).
  struct Snapshot { int armed; uint32_t lo, hi; };
  Snapshot save() const                     { return Snapshot{ mArmed, mLo, mHi }; }
  void     restore(Snapshot s)              { mArmed = s.armed; mLo = s.lo; mHi = s.hi; }
  void     open()                           { mArmed = 1; mLo = 0xFFFFFFFFu; mHi = 0; }
  // After close-of-inner: restore the outer's snapshot, then merge the just-closed inner span into it.
  void     restoreMerge(Snapshot outer, uint32_t inner_lo, uint32_t inner_hi) {
    restore(outer);
    if (inner_hi > inner_lo) {
      if (inner_lo < mLo) mLo = inner_lo;
      if (inner_hi > mHi) mHi = inner_hi;
    }
  }

  // Current session's captured span (only valid while armed / for the just-closed session before nesting
  // moves on). Returns true iff non-empty.
  bool current(uint32_t* lo, uint32_t* hi) const {
    if (mHi > mLo) { if (lo) *lo = mLo; if (hi) *hi = mHi; return true; }
    return false;
  }

private:
  int      mArmed = 0;
  uint32_t mLo    = 0xFFFFFFFFu;
  uint32_t mHi    = 0;
};

// RAII object scope — the canonical form used by the render walk. Opens a fresh nested session on
// construction, restores + merges into the outer on close.
class PktSpanSession {
public:
  explicit PktSpanSession(Core* c);
  int close(uint32_t* lo, uint32_t* hi);   // 1 iff captured non-empty; caller uses that to tag depth.
private:
  PktSpan* mPs;
  PktSpan::Snapshot mOuter;
};
