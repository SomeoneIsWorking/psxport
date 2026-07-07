// class FfSpan — PSXPORT_BDTAG per-frame builder-span attribution, one instance per Game.
//
// later-238 BACKDROP ATTRIBUTION: record each field-frame build call's pool-write span so the gp0
// OT-walk classifier (gpu_native.cpp) can attribute a DEFERRED prim (e.g. the tp(576,256) sea backdrop)
// to the call that BUILT it — reliable where per-pass tags / WWATCH-pc / pool-node-addresses are not.
// The span table persists across the present (which classifies the prior frame's OT) because it is
// reset only at the TOP of the next frame (resetFrame from native_step_frame). `lookup(addr)` returns
// the builder name (latest-span-wins). begin()/end(nm) bracket a frame phase; NESTABLE via a small
// stack of PktSpan snapshots. (Was the extern "C" ffspan_* free functions + file-scope statics in
// engine.cpp.) Reached as `c->game->ffspan.<...>`; back-pointer `core` wired in Game's ctor.
#pragma once
#include <stdint.h>
#include "pkt_span.h"
class Core;

class FfSpan {
public:
  Core* core = nullptr;

  int bdtagOn();                      // lazy PSXPORT_BDTAG latch (was bdtag_on())
  // EARLIEST-first = INNERMOST-wins: an inner bracket ends (is recorded) before its outer, and outer
  // spans merge in their children, so the first containing span in record order is the tightest
  // (real) builder.
  const char* lookup(uint32_t a) const;
  void resetFrame();                  // frame top: clear the span table + the bracket stack
  void begin();                       // NESTABLE: save outer, open a fresh empty session
  void end(const char* nm);           // close the session, record its span under `nm`, merge into outer
  void dump(uint32_t a);              // one-time: show the recorded spans vs an unattributed address
  // Record one closed span (used by end() and the FFS macro in engine.cpp).
  void record(const char* nm, uint32_t lo, uint32_t hi) {
    if (mN < kMaxSpans) { mSpans[mN].name = nm; mSpans[mN].lo = lo; mSpans[mN].hi = hi; mN++; }
  }

private:
  static constexpr int kMaxSpans = 40;
  struct Span { const char* name; uint32_t lo, hi; };
  Span mSpans[kMaxSpans] = {};
  int  mN = 0;
  int  mBdTag = -1;
  PktSpan::Snapshot mStk[8] = {};
  int  mSp = 0;
  int  mDumped = 0;
};
