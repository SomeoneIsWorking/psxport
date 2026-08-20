// class OtAttr — OT/GTE SUBMISSION ATTRIBUTION tool (`debug otattr`).
//
// MOTIVATION (USER 2026-07-14, serves bug #45 + general render investigations): trace every guest-
// submitted primitive (packet-pool store) and every GTE RTPS/RTPT execution back to (a) the guest
// FUNCTION that submitted it and (b) the world-object NODE whose render-walk scope was open at the
// time. "Which fn+node drew this dialog-panel packet / this gem quad" without hand-walking backwards
// through addresses.
//
// DIAGNOSTIC ONLY. Zero behavior change when the `otattr` debug channel is off (one interned
// lucent::Channel test gates every method). Never writes guest memory;
// only reads Core::mem_r* / Render::diag / InterpDiag's otattr shadow stack.
//
// Two independent tables, both per-Core (SBS runs two: no cross-contamination) and both keyed to the
// CURRENT render frame (GpuState::s_frame) with a LAZY reset on first touch of a new frame — so the
// data for the just-finished frame is still readable any time before the NEXT frame's first store/GTE
// op, which is exactly when a REPL command run between `run N` steps executes. Sized statically;
// overflow is COUNTED and reported rather than growing unbounded or silently dropping.
//
//   1. Packet-pool store spans — every guest store landing in the shared packet pool (the
//      GameConfig-derived window, render_noise.h; Tomba!2's is [0x800BFE68, 0x800E7E68), and a game
//      that has not RE'd its pool gets NO window and is told so) is attributed to {emitter fn = the
//      OTATTR SHADOW STACK top (InterpDiag::otattrTop(), the innermost guest fn reached via an
//      INDIRECT/jalr rec_dispatch — see interp_diag.h), caller fn = one frame below that, node =
//      Render::diag.currentNode()}. Consecutive stores with the SAME (fn, node) attribution are
//      COALESCED into one growing [lo,hi) span so one quad's
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
#include <lucent/log.h> // lucent::Channel — the inline armed test below
#include <stdint.h>
class Core;

// The `otattr` channel as an INTERNED HANDLE. This is the one place in the framework that has earned
// one: the gate below runs on every guest store. A `lucent::Channel` is constant-initialised (no
// guard variable, no static-init order), caches its answer next to a stamp of the enabled-channel
// set, and is invalidated globally by any enable_channels() — so `debug otattr` at the REPL still
// takes effect on the very next store, which is the property the hand-rolled generation counter this
// replaces existed to provide. Measured in lucent: 0.66 ns/gate against 19.4 ns for the
// channel-name-lookup form.
inline const lucent::Channel g_otattr_channel{"otattr"};
// `otchain` is SEPARATE from `otattr` on purpose: it walks the whole shadow stack per new span, which is
// real per-store work, and it answers a one-off structural question rather than being a run-time trace.
inline const lucent::Channel g_otchain_channel{"otchain"};

// THE SECOND REASON TO TRACK SPANS: the graphics-producer DB's GUEST leg needs the same attribution the
// `otattr` channel produces, but during ORDINARY PLAY with no debug channel on — the DB's whole premise
// is that playing populates it (docs/plans/graphics-producer-db.md). So the hot-path gate opens for
// EITHER reason. This is a plain bool rather than a Channel because it is not a diagnostic the user
// enables; it is armed by the census's own wiring, and it is read on every guest store, so the cost of
// widening the gate is MEASURED and recorded next to the arming call — not assumed to be free. The
// existing comment below records what the out-of-line version cost (2.54% of total CPU) and is the
// number any change here has to beat.
// DEFAULT ON, and that is a measured decision rather than an optimistic one. The DB's premise is that
// ORDINARY PLAY populates it, so an opt-in flag would leave exactly the data source that matters
// uncounted. Priced on Tomba!2, 300 frames headless, three runs each, identical script:
//     gate closed (before): 77.7 / 77.6 / 77.7 s
//     gate open  (armed)  : 77.9 / 77.9 / 77.7 s     => +0.26%
// against the 2.54% of total CPU the OUT-OF-LINE version of this gate once cost (see below). And the
// work is real, not optimised away: the same run records 10,188 spans with 0 overflow, which is printed
// at run end precisely so "no cost" can never be mistaken for "no work happened".
inline bool g_producer_census_armed = true;

class OtAttr {
public:
  // A single terrain-heavy field frame can produce well over 4096 attribution-distinct spans (terrain
  // tiles interleave E1/E2 texpage-state words between quads, which breaks the lo/hi coalescing even
  // when the fn/node attribution is identical) — sized generously since this is diagnostic-only memory,
  // live only while the `otattr` channel is enabled.
  static constexpr int SPAN_CAP = 65536;
  static constexpr int GTE_CAP = 512;

  // `pc` = the guest fn most recently ENTERED when this store happened. Every recompiled wrapper opens
  // with `c->pc = 0x<its own address>`, so unlike `fn` (which comes from the indirect-dispatch shadow
  // stack and is 0 whenever nothing was dispatched indirectly) this is populated for DIRECT calls too.
  // It is a WEAKER claim than `fn`: nothing restores it when a nested call returns, so a store made by A
  // after A called B reads as B. Recorded alongside `fn` rather than instead of it, and reported
  // separately, so the difference stays visible instead of one silently standing in for the other.
  // `claimed` = the frame in this store's call chain that a NATIVE producer already keys a row at
  // (ProducerCensus::claims), or 0 when no frame in the searched window is claimed. THE JOIN BETWEEN THE
  // TWO LEGS: `fn` is the innermost EMITTER, which is not where a native row is keyed, so attributing the
  // guest leg to `fn` puts the legs in disjoint rows. Kept ALONGSIDE `fn` rather than replacing it, so
  // "the emitter" and "the row it belongs to" stay separately readable — collapsing them is how a
  // resolution bug becomes invisible.
  struct Span {
    uint32_t lo, hi;
    uint32_t fn, caller, node, pc, claimed;
  };

  // The chain walk is BOUNDED — an unclaimed chain would otherwise pay a full-depth scan on every new
  // span, on a path that is armed by default. 8 is not a guess: the otchain measurement found every claim
  // within 3 frames of the top (max observed 3), so 8 is well past the observed need. A claim found AT the
  // limit is COUNTED and warned about, because a silently-too-small window looks exactly like an effect
  // with no native producer.
  static constexpr int CLAIM_SEARCH_DEPTH = 8;
  struct GteBucket {
    uint32_t fn, node, count;
  };

  // Called from Core::mem_w8/16/32 (mem.cpp) for EVERY guest store — no-op unless the `otattr` channel
  // is enabled (checked internally — a hot-path early-out) and the address falls in
  // the packet-pool range.
  // Called on EVERY guest store (Core::mem_w* -> pkt_track). The armed test is inline so the common
  // case is a load and a compare with no call at all — it was three nested out-of-line calls
  // (trackStore -> cfg_dbg_generation -> bootstrap_once) to conclude it had nothing to do, measuring
  // 2.54% of total CPU on its own. lucent::Channel is that same generation-stamped cache, done once
  // in the logging library instead of hand-rolled here against a counter cfg.cpp had to export.
  void trackStoreSlow(Core *c, uint32_t addr, uint32_t bytes);
  inline void trackStore(Core *c, uint32_t addr, uint32_t bytes) {
    // Steady state with both off: two relaxed loads and two compares, still no call.
    if (!g_otattr_channel && !g_producer_census_armed) {
      return;
    }
    trackStoreSlow(c, addr, bytes);
  }

  // Called from gte_op's RTPS/RTPT branch (gte_beetle.cpp) — aggregates a call count per (fn, node).
  void trackGte(Core *c);

  // REPL/diagnostic readback — NOT hot path, may scan linearly.
  bool lookupStore(uint32_t addr, Span *out) const;
  int spanCount() const {
    return mSpanCount;
  }
  int spanOverflow() const {
    return mSpanOverflow;
  }
  const Span *spanAt(int i) const {
    return &mSpans[i];
  }
  int gteCount() const {
    return mGteCount;
  }
  int gteOverflow() const {
    return mGteOverflow;
  }
  const GteBucket *gteAt(int i) const {
    return &mGte[i];
  }
  uint32_t frame() const {
    return mFrame;
  }

  // --- LAST-WRITER PROVENANCE (watched regions) ---

  // Up to 8 regions; 64 KB of word-granular last-writer records total across all of them (static, so
  // this is diagnostic memory that's live regardless of whether `otattr` is on — cheap, always allocated
  // like the span/GTE tables above).
  static constexpr int WATCH_SLOTS = 8;
  static constexpr int WATCH_CAP_BYTES = 65536;
  static constexpr int WATCH_CAP_WORDS = WATCH_CAP_BYTES / 4;

  // Physical (0x1FFFFFFF-masked) address range — works uniformly for main RAM (KUSEG/KSEG0/KSEG1 all
  // mask to the same 0x000xxxxx..0x1FFFFFxx physical range) AND scratchpad (0x1F800000-0x1F8003FF,
  // which is NOT mirrored across segments the way main RAM is — see trackStore's `phys` comment).
  struct WatchRegion {
    uint32_t lo = 0, hi = 0, wordBase = 0;
    bool active = false;
  };
  // `frame == NO_FRAME` is the never-written state of a freshly carved region's slice (see
  // watchRegister) — the negative that must stay distinguishable from a recorded write.
  struct WordRec {
    uint32_t fn = 0, caller = 0, frame = 0xFFFFFFFFu /* NO_FRAME */;
  };

  static constexpr int FNSTAT_CAP = 256;
  static constexpr int FNSTAT_PAGES = 8; // distinct 4KB dest pages tracked per fn before "overflow" (many)
  struct FnStoreStat {
    uint32_t fn = 0, count = 0;
    uint32_t pages[FNSTAT_PAGES] = {};
    int pageCount = 0;
    bool pageOverflow = false;
  };

  // Register a watched region [addr, addr+len). Returns the slot index, or -1 if all WATCH_SLOTS are
  // used or the WATCH_CAP_WORDS word budget is exhausted (both counted so `otattr watch` can report why).
  int watchRegister(uint32_t addr, uint32_t len);
  int watchSlotCount() const {
    return mWatchCount;
  }
  int watchWordsUsed() const {
    return mWatchWordsUsed;
  }
  int watchOverflow() const {
    return mWatchOverflow;
  }
  const WatchRegion *watchAt(int i) const {
    return &mWatch[i];
  }

  // Word-granular last-writer lookup. `addr` may be any address inside a watched region (rounded down
  // to its containing word). Returns false if `addr` isn't inside ANY watched region.
  bool watchLookup(uint32_t addr, WordRec *out, uint32_t *wordAddrOut = nullptr) const;

  const FnStoreStat *fnStatFind(uint32_t fn) const;
  int fnStatCount() const {
    return mFnStatCount;
  }
  const FnStoreStat *fnStatAt(int i) const {
    return &mFnStat[i];
  }

  // Bound the span table's lifetime to ONE LOGIC FRAME, driven by the frame loop.
  //
  // WHY THIS IS PUBLIC AND CALLED FROM OUTSIDE. The table used to be reset off `gpu.s_frame`, which
  // counts PRESENTS, not logic frames. Measured on the gte render path: 60 logic frames advanced
  // s_frame to 3, so the reset almost never fired, the table saturated at SPAN_CAP and then reported
  // 140,153 overflows — and worse, every lookup after that matched a STALE early-boot span whose
  // emitter fn was 0, which is why the guest leg reported 16,384 prims as span-no-fn and attributed
  // NONE. A span table is only meaningful for the frame whose packets it describes, so the frame loop
  // says when a frame begins and this is the one clock that matters.
  void beginLogicFrame(uint32_t frame) {
    resetIfNewFrame(frame);
  }

  // Run-end verdict for the frame-loop contract. Stores before a game's loop starts are legitimate
  // boot activity, so stampFrame records them but does not accuse the loop immediately. The verdict
  // becomes meaningful only after the run had a chance to call beginLogicFrame.
  bool frameContractSatisfied() const {
    return mFrame != NO_FRAME || mPreFrameStamps == 0;
  }
  uint64_t preFrameStampCount() const {
    return mPreFrameStamps;
  }
  void reportFrameContract(const char *context) const;

  // The idle value of every frame field below: "no frame has been declared yet" for the table clocks,
  // and "this word was never written" for a watch record. Named because two of the three used to be a
  // bare 0xFFFFFFFF literal and a reader could not tell whether a report showing it meant "nothing
  // happened" or "the clock was never set".
  static constexpr uint32_t NO_FRAME = 0xFFFFFFFFu;

  // THE ONE CLOCK EVERYTHING HERE STAMPS WITH — and the reason this class no longer touches `Game`.
  //
  // ROOT CAUSE IT REPLACES (fixed 2026-08-12): trackStoreSlow and trackGte read `c->game->gpu.s_frame`.
  // That was wrong twice over. (1) It made a CORE PRIMITIVE hard-require the whole machine: `Core::game`
  // is nullptr until `Game`'s constructor sets it, `g_producer_census_armed` is default TRUE, so
  // `Core::mem_w32` on a Core-alone embedder reached this path and null-dereferenced — a plain guest
  // store crashing with no game bound. Measured victim: `psxport_smoke` (rc=139, zero output).
  // `tests/test_overlay_reloc.cpp` is a bare-Core embedder too but writes no memory at all, so it was
  // latently exposed rather than actually broken. (2) `gpu.s_frame`
  // counts PRESENTS while `mFrame` (driven by beginLogicFrame) counts LOGIC FRAMES, so ONE function
  // stamped its span table off one clock and its watch + per-fn tables off another. The measured
  // saturation that made beginLogicFrame exist in the first place (60 logic frames advanced s_frame to
  // 3, so the reset almost never fired) applied verbatim to those other two tables.
  //
  // A Core that no frame loop drives has a genuinely undefined frame, and that case is WARNED AT
  // RUN END rather than quietly stamped. Warning here would falsely accuse normal boot stores before
  // a healthy game loop has had a chance to start. An unstamped table must not be mistakable for a stamped one. It
  // returns 0 (not NO_FRAME) because NO_FRAME is also WordRec's "never written" — stamping a real write with it would
  // make a recorded store indistinguishable from an untouched word.
  uint32_t stampFrame();

  // `PSXPORT_DEBUG=otchain` — WHAT THE WHOLE CALL CHAIN IS at a packet-pool store, sampled.
  //
  // It exists to settle ONE question with data instead of reasoning: the producer DB's guest leg keys a
  // prim at the chain's TOP (the innermost emitter), while its native rows are keyed at the HANDLER/PASS
  // frame — so the two legs land in disjoint rows and compare nothing. The proposed fix is to key on "the
  // frame in the chain that a producer row claims", and that fix rests entirely on the premise THE HANDLER
  // FRAME IS ACTUALLY ON THE CHAIN at store time. This prints the chains so the premise can be checked
  // rather than assumed.
  //
  // Capped BY NOVELTY, not by count: one sample per distinct (top, depth) pair, so a chain shape that
  // occurs once is as visible as one that occurs 78,000 times. `mChainSeen` is the denominator and
  // `mChainDropped` the shapes lost to a full table — a report with a full table is explicitly incomplete.
  static constexpr int CHAIN_SLOTS = 128; // distinct chain SHAPES kept
  // 32, raised from 20 on 2026-08-12: the deepest observed chain is 28 frames, and at 20 a reader had to
  // RECONSTRUCT the tail by splicing two shapes together to answer "is there a claim further out". A
  // diagnostic that requires arithmetic to read is one people will read wrong.
  static constexpr int CHAIN_FRAMES = 32; // frames recorded per shape, innermost first
  struct ChainSample {
    uint32_t top;  // otattrTop() — the key the guest leg uses today
    int depth;     // full guest depth (may exceed what was recorded, and may exceed the cap)
    int nframes;   // how many of `frames` are populated
    uint64_t hits; // stores that matched this shape
    uint32_t frames[CHAIN_FRAMES];
  };
  uint32_t resolveClaimedFrame(Core *c);
  void sampleChain(Core *c);
  // Reported at run end alongside the census. `claims`/`nclaims` = the addresses a producer row is keyed
  // at, so the report can state, per shape, WHETHER any frame in the chain is one of them — that is the
  // measurement, and a chain matching none is the interesting case, not a blank.
  void reportChains(const uint32_t *claims, int nclaims) const;
  // The claim set AS THE RESOLVER SEES IT. reportChains used to be handed a list derived from this run's
  // census rows, while resolveClaimedFrame consults the PERSISTED set — so on a guest leg the report
  // measured a different thing from the resolver and printed "0 of 29 claimed" while the resolver was
  // joining 266,760 spans. It refused loudly rather than lying, which is why this was findable, but a
  // report and the mechanism it reports on must not read from two sources.
  // Run-end accounting for the claim resolution itself, so a join rate is never read off row counts alone.
  uint64_t claimResolved() const {
    return mClaimResolved;
  }
  uint64_t claimUnresolved() const {
    return mClaimUnresolved;
  }
  uint64_t claimAtLimit() const {
    return mClaimAtLimit;
  }

private:
  void resetIfNewFrame(uint32_t frame);
  void trackWatch(uint32_t fn, uint32_t caller, uint32_t phys, uint32_t bytes, uint32_t frame);
  void recordFnStat(uint32_t frame, uint32_t fn, uint32_t phys);

  uint32_t mFrame = NO_FRAME;
  uint64_t mPreFrameStamps = 0;
  Span mSpans[SPAN_CAP] = {};
  int mSpanCount = 0;
  int mSpanOverflow = 0;
  // Is mSpans address-sorted? Maintained on insert (never assumed) so lookupStore can binary-search;
  // false falls back to the reverse linear scan, which is always correct.
  bool mSpansSorted = true;

  uint32_t mGteFrame = NO_FRAME;
  GteBucket mGte[GTE_CAP] = {};
  int mGteCount = 0;
  int mGteOverflow = 0;

  WatchRegion mWatch[WATCH_SLOTS] = {};
  int mWatchCount = 0;
  int mWatchWordsUsed = 0;
  int mWatchOverflow = 0;
  WordRec mWatchWords[WATCH_CAP_WORDS] = {}; // pooled backing store, indexed via wordBase + offset

  ChainSample mChains[CHAIN_SLOTS] = {};
  int mChainCount = 0;
  uint64_t mChainSeen = 0;       // stores offered to the sampler — the denominator
  uint64_t mChainDropped = 0;    // novel shapes lost because the table was full
  uint64_t mChainBlind = 0;      // chains the shadow stack could not see (depth over OTATTR_CAP)
  uint64_t mClaimAtLimit = 0;    // claims found at the last searched frame — the window may be too small
  uint64_t mClaimResolved = 0;   // spans whose chain yielded a claimed frame
  uint64_t mClaimUnresolved = 0; // spans with no claimed frame in the window — the honest "no native producer"

  uint32_t mFnStatFrame = NO_FRAME;
  FnStoreStat mFnStat[FNSTAT_CAP] = {};
  int mFnStatCount = 0;
  int mFnStatOverflow = 0;
};
