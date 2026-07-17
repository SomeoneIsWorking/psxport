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
// Reached as `core->rsub.otAttr`. The REPL `otattr` command (repl.cpp) re-walks the CURRENT OT
// read-only (no gpu_gp0 side effects) and looks up each packet's pool address in table 1, printing the
// GTE histogram from table 2 alongside.
//
// LAST-WRITER PROVENANCE extension (USER 2026-07-14, follow-up to the census's open question: call-path
// attribution alone names only the BATCHER when a game copies many objects' quads through one shared
// staging buffer before emission — the object identity is lost at the RAM copy). Answers "who wrote this
// WORD", independent of call-flow:
//
//   3. Watched-region registry (`otattr watch <addr> <len>`, up to WATCH_SLOTS regions, WATCH_CAP_WORDS
//      words total across all regions) — a per-WORD (4-byte) last-writer table {fn = otattr shadow-stack
//      top at store time, caller, frame}. Any address, not just the packet pool — this is what lets it
//      watch scratchpad (GTE staging) or a game-specific staging buffer once one is suspected. Fed from
//      the SAME store-interception point as table 1 (Core::mem_w8/16/32 -> pkt_track -> trackStore), so
//      it inherits the same "off costs one branch" guarantee and the same scratchpad/native-code caveats
//      documented at trackStore's definition.
//
//   4. Per-fn store-count stat for the current frame (`mFnStat`, fed unconditionally alongside the watch
//      table) — total store count + up to FNSTAT_PAGES distinct 4KB destination pages touched. Powers the
//      `otattr trace <addr>` heuristic: a writer fn that touches many distinct pages in one frame LOOKS
//      like a copy loop (batching many sources into one buffer), which is the census's actual scenario.
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

  // --- LAST-WRITER PROVENANCE (watched regions) ---

  // Up to 8 regions; 64 KB of word-granular last-writer records total across all of them (static, so
  // this is diagnostic memory that's live regardless of whether `otattr` is on — cheap, always allocated
  // like the span/GTE tables above).
  static constexpr int WATCH_SLOTS     = 8;
  static constexpr int WATCH_CAP_BYTES = 65536;
  static constexpr int WATCH_CAP_WORDS = WATCH_CAP_BYTES / 4;

  // Physical (0x1FFFFFFF-masked) address range — works uniformly for main RAM (KUSEG/KSEG0/KSEG1 all
  // mask to the same 0x000xxxxx..0x1FFFFFxx physical range) AND scratchpad (0x1F800000-0x1F8003FF,
  // which is NOT mirrored across segments the way main RAM is — see trackStore's `phys` comment).
  struct WatchRegion { uint32_t lo = 0, hi = 0, wordBase = 0; bool active = false; };
  struct WordRec     { uint32_t fn = 0, caller = 0, frame = 0xFFFFFFFFu; };

  static constexpr int FNSTAT_CAP   = 256;
  static constexpr int FNSTAT_PAGES = 8;   // distinct 4KB dest pages tracked per fn before "overflow" (many)
  struct FnStoreStat {
    uint32_t fn = 0, count = 0;
    uint32_t pages[FNSTAT_PAGES] = {};
    int      pageCount = 0;
    bool     pageOverflow = false;
  };

  // Register a watched region [addr, addr+len). Returns the slot index, or -1 if all WATCH_SLOTS are
  // used or the WATCH_CAP_WORDS word budget is exhausted (both counted so `otattr watch` can report why).
  int watchRegister(uint32_t addr, uint32_t len);
  int watchSlotCount() const { return mWatchCount; }
  int watchWordsUsed() const { return mWatchWordsUsed; }
  int watchOverflow()  const { return mWatchOverflow; }
  const WatchRegion* watchAt(int i) const { return &mWatch[i]; }

  // Word-granular last-writer lookup. `addr` may be any address inside a watched region (rounded down
  // to its containing word). Returns false if `addr` isn't inside ANY watched region.
  bool watchLookup(uint32_t addr, WordRec* out, uint32_t* wordAddrOut = nullptr) const;

  const FnStoreStat* fnStatFind(uint32_t fn) const;
  int fnStatCount() const { return mFnStatCount; }
  const FnStoreStat* fnStatAt(int i) const { return &mFnStat[i]; }

private:
  void resetIfNewFrame(uint32_t frame);
  void trackWatch(uint32_t fn, uint32_t caller, uint32_t phys, uint32_t bytes, uint32_t frame);
  void recordFnStat(uint32_t frame, uint32_t fn, uint32_t phys);

  uint32_t mFrame = 0xFFFFFFFFu;
  Span     mSpans[SPAN_CAP] = {};
  int      mSpanCount = 0;
  int      mSpanOverflow = 0;

  uint32_t mGteFrame = 0xFFFFFFFFu;
  GteBucket mGte[GTE_CAP] = {};
  int       mGteCount = 0;
  int       mGteOverflow = 0;

  WatchRegion mWatch[WATCH_SLOTS] = {};
  int         mWatchCount = 0;
  int         mWatchWordsUsed = 0;
  int         mWatchOverflow = 0;
  WordRec     mWatchWords[WATCH_CAP_WORDS] = {};   // pooled backing store, indexed via wordBase + offset

  uint32_t    mFnStatFrame = 0xFFFFFFFFu;
  FnStoreStat mFnStat[FNSTAT_CAP] = {};
  int         mFnStatCount = 0;
  int         mFnStatOverflow = 0;
};
