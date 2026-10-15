// class OtAttr — OT/GTE SUBMISSION ATTRIBUTION tool (`debug otattr`).
//
// MOTIVATION (USER 2026-07-14, serves bug #45 + general render investigations): trace every guest-
// submitted primitive (packet-pool store) and every GTE RTPS/RTPT execution back to the guest
// FUNCTION that submitted it.
//
// DIAGNOSTIC ONLY. Zero behavior change when the `otattr` debug channel is off (one interned
// lucent::Channel test gates every method). Never writes guest memory;
// only reads Core::mem_r* and the call-attribution stack.
//
// Two independent tables, both per-Core (SBS runs two: no cross-contamination) and both keyed to the
// CURRENT logic frame with a LAZY reset on first touch of a new frame — so the
// data for the just-finished frame is still readable any time before the NEXT frame's first store/GTE
// op, which is exactly when a REPL command run between `run N` steps executes. Sized statically;
// overflow is COUNTED and reported rather than growing unbounded or silently dropping.
//
//   1. Packet-pool store spans — every guest store landing in the title's packet pool
//      (GameRuntime::packetPool) is attributed to {emitter fn = the
//      OTATTR SHADOW STACK top (InterpDiag::otattrTop(), the innermost guest fn reached via an
//      INDIRECT/jalr guest dispatch — see interp_diag.h), caller fn = one frame below that}.
//      Consecutive stores with the SAME attribution are
//      COALESCED into one growing [lo,hi) span so one quad's
//      handful of header/vertex/color words collapse to a single entry instead of one-per-word —
//      this is what keeps the static cap (OT_ATTR_CAP) from blowing out on a normal frame.
//
//   2. GTE RTPS/RTPT per-fn call counts for the frame (not per-call — aggregated), fed from
//      Gte::execute after each RTPS/RTPT (runtime/psx/cpu/gte_beetle.cpp).
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
#include "guest_address_range.h"
#include "guest_packet_pool_windows.h"
#include "producer_census.h"
#include <functional>
#include <lucent/log.h> // lucent::Channel — the inline armed test below

#include <stdint.h>
#include <string_view>
class Core;

// The `otattr` channel as an INTERNED HANDLE. This is the one place in the framework that has earned
// one: the gate below runs on every guest store. A `lucent::Channel` is constant-initialised (no
// guard variable, no static-init order), caches its answer next to a stamp of the enabled-channel
// set, and is invalidated globally by any enable_channels() — so `debug otattr` at the REPL still
// takes effect on the very next store, which is the property the hand-rolled generation counter this
// replaces existed to provide. Measured in lucent: 0.66 ns/gate against 19.4 ns for the
// channel-name-lookup form.
inline constexpr lucent::Channel g_otattr_channel{"otattr"};

// THE SECOND REASON TO TRACK SPANS: the graphics-producer DB's GUEST leg needs the same attribution the
// `otattr` channel produces, but during ORDINARY PLAY with no debug channel on — the DB's whole premise
// is that playing populates it. So the hot-path gate opens for
// EITHER reason. This is a plain bool rather than a Channel because it is not a diagnostic the user
// enables; it is armed by the census's own wiring, and it is read on every guest store, so the cost of
// widening the gate is MEASURED and recorded next to the arming call — not assumed to be free. The
// existing comment below records what the out-of-line version cost (2.54% of total CPU) and is the
// number any change here has to beat.
// DEFAULT ON, and that is a measured decision rather than an optimistic one. The DB's premise is that
// ORDINARY PLAY populates it, so an opt-in flag would leave exactly the data source that matters
// uncounted.
//
// THE "+0.26%" PRICE RECORDED HERE WAS WRONG, and is corrected rather than deleted so nobody
// re-derives it. It read:
//     gate closed (before): 77.7 / 77.6 / 77.7 s
//     gate open  (armed)  : 77.9 / 77.9 / 77.7 s     => +0.26%
// 300 frames in 77.7 s is 3.9 fps — a workload so dominated by something else that almost any fixed
// cost measures as free. Re-priced 2026-08-20 on an UNPACED 3D field scene (1,100 frames of
// replays/bugs/ingame-options-page.pad, clang, three runs each), using the PSXPORT_PRODUCERS knob
// that had to be added because the arm was a hardcoded `inline bool = true` with no way to A/B it:
//     armed   5.662 / 5.789 / 5.757 s   mean 5.736
//     off     4.784 / 4.631 / 4.695 s   mean 4.703     => the census costs 18.0%
// A host profile of the same scene agrees independently: OtAttr::resolveClaimedFrame 13.31% +
// trackStoreSlow 3.91% = 17.2%. Two methods, one answer.
//
// This is NOT an argument for deleting the census — the USER asked for the producer database, and a
// diagnostic that answers "which native producer drew this" is worth real time. It is an argument for
// knowing what it costs, and for being able to switch it off when measuring something else.
// against the 2.54% of total CPU the OUT-OF-LINE version of this gate once cost (see below). And the
// work is real, not optimised away: the same run records 10,188 spans with 0 overflow, which is printed
// at run end precisely so "no cost" can never be mistaken for "no work happened".
// Set once per Game construction from PSXPORT_PRODUCERS (game.cpp), which defaults TRUE so behaviour
// is unchanged; Game's constructor is on every runtime's boot route. It stays a plain global rather than a CVar read
// because trackStore's inline gate is on the path of EVERY guest store — the whole point of that gate is that it is two
// relaxed loads.
inline bool g_producer_census_armed = true;

class OtAttr {
public:
  // The `otattr watch` / `otattr who` last-writer sub-commands, one implementation for every control
  // transport (the REPL and the debug server). `emit` is the transport's sink. Returns true when `line`
  // named one of them and was consumed; false means the caller keeps its own `otattr` behaviour.
  bool runLastWriterCommand(std::string_view line, const std::function<void(std::string_view)> &emit);
  // A single terrain-heavy field frame can produce well over 4096 attribution-distinct spans (terrain
  // tiles interleave E1/E2 texpage-state words between quads, which breaks the lo/hi coalescing even
  // when the fn attribution is identical) — sized generously since this is diagnostic-only memory,
  // live only while the `otattr` channel is enabled.
  static constexpr int SPAN_CAP = 65536;
  static constexpr int GTE_CAP = 512;

  // `pc` = the guest fn most recently ENTERED when this store happened. Every guest wrapper opens
  // with `c->pc = 0x<its own address>`, so unlike `fn` (which comes from the indirect-dispatch shadow
  // stack and is 0 whenever nothing was dispatched indirectly) this is populated for DIRECT calls too.
  // It is a WEAKER claim than `fn`: nothing restores it when a nested call returns, so a store made by A
  // after A called B reads as B. Recorded alongside `fn` rather than instead of it, and reported
  // separately, so the difference stays visible instead of one silently standing in for the other.
  // `claimed` = the first frame of the store's call chain that a registered producer owns, or 0; the
  // census row a store belongs to when its emitter `fn` is a helper the producer calls.
  struct Span {
    uint32_t lo, hi;
    uint32_t fn, caller, pc, claimed;
  };

  // Call-chain frames searched for a producer, innermost first.
  static constexpr int CLAIM_SEARCH_DEPTH = 8;
  struct GteBucket {
    uint32_t fn, count;
  };

  // Called from Core::mem_w8/16/32 (mem.cpp) for EVERY guest store; the armed test is inline so the
  // common case is a load and a compare with no call.
  void trackStoreSlow(Core *c, GuestSpan store);
  inline void trackStore(Core *c, GuestSpan store) {
    // Steady state with both off: two relaxed loads and two compares, still no call.
    if (!g_otattr_channel && !g_producer_census_armed) {
      return;
    }
    trackStoreSlow(c, store);
  }

  // Called from Gte::execute after RTPS/RTPT — aggregates a call count per fn.
  void trackGte(Core *c);

  // REPL/diagnostic readback — NOT hot path, may scan linearly.
  bool lookupStore(uint32_t addr, Span *out) const;
  // The submitter of the packet whose first GP0 word is at `packetAddress`, from this frame's spans.
  psx::debug::ProducerAttribution submitterOf(Core *c, uint32_t packetAddress) const;
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

  // Register a watched region. Returns the slot index, or -1 if all WATCH_SLOTS are used or the
  // WATCH_CAP_WORDS word budget is exhausted (both counted so `otattr watch` can report why).
  int watchRegister(GuestSpan region);
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

  // The frame loop declares each logic frame; spans only describe the frame whose packets they hold.
  void beginLogicFrame(uint32_t frame) {
    resetIfNewFrame(frame);
    mPoolStale = true;
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

  // The logic frame every table stamps with, or 0 before the frame loop declared one (warned at run end).
  uint32_t stampFrame();

  // The first frame of the store's call chain that a registered producer owns, or 0.
  uint32_t resolveClaimedFrame(Core *c);

private:
  // Out of line: the per-store path only tests mPoolStale.
  void refreshPool(Core *c);

  psx::gpu::PacketPoolExtent mPool;
  bool mPoolStale = true;
  bool mWarnedNoPool = false;

  void resetIfNewFrame(uint32_t frame);
  // Who made a store: the attributed function, its caller and the frame stamp.
  struct StoreOrigin {
    uint32_t fn;
    uint32_t caller;
    uint32_t frame;
  };
  void trackWatch(StoreOrigin origin, GuestSpan physical);
  void recordFnStat(StoreOrigin origin, uint32_t phys);

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

  // resolveClaimedFrame's answers, keyed on the exact visible call-chain prefix; valid for one
  // producer revision of the dispatcher.
  static constexpr int RCACHE_SLOTS = 256; // power of two
  struct ResolveEntry {
    uint32_t prefix[CLAIM_SEARCH_DEPTH] = {};
    int depth = -1; // -1 = empty slot
    uint32_t result = 0;
  };
  ResolveEntry mResolveCache[RCACHE_SLOTS];
  uint64_t mResolveCacheRevision = ~0ull; // producer revision the cache was built against

  uint32_t mFnStatFrame = NO_FRAME;
  FnStoreStat mFnStat[FNSTAT_CAP] = {};
  int mFnStatCount = 0;
  int mFnStatOverflow = 0;
};
