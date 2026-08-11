// OtAttr implementation — see ot_attr.h for the design. Separated from the header so the header can
// stay a forward-declared-Core, dependency-light include.
#include "ot_attr.h"
#include "render_node.h"   // cur_render_node — same node fallback the native submit path itself uses
#include "core.h"
#include "game.h"
#include "render_noise.h"   // THE one GameConfig-derived definition of the pool / OT / pool-ptr windows
#include <lucent/log.h>

// THE PACKET POOL RANGE COMES FROM THE GAME, not from here. It used to be two file-scope constants
// holding Tomba!2's addresses (0x800BFE68..0x800E7E68) inside game-agnostic framework code — so on any
// other consumer this whole table silently matched nothing and reported no attributions, which is
// indistinguishable from "the guest submitted no packets". GameConfig::packetPoolBase/Stride already
// carry it (native_step_frame's per-frame OT/pool dance uses them); this reads the same fields.
//
// TWO PARITY POOLS: base + 2*stride is the pair, which reproduces Tomba!2's old constants exactly
// (0x800BFE68 + 2*0x14000 = 0x800E7E68) — verified against them rather than re-derived.
//
// A GAME THAT HAS NOT RE'd ITS POOL LEAVES THE FIELDS 0 (spyro, spider1 — an honest zero with a TODO,
// per their own rules). Then this feed CANNOT attribute anything, and it says so once instead of
// producing an empty table that reads like a measurement.
//
// The arithmetic moved to render_noise.h (2026-08-11) because dualcore.cpp and selftest.cpp each had
// their OWN copy of it with Tomba!2's literals still in them — three copies meant fixing one left two
// lying. This function is now just the pool window of that shared mask.
namespace {
struct PoolRange { uint32_t lo, hi; bool known; };
PoolRange pool_range(Core* c) {
  const RenderNoiseMask m = RenderNoiseMask::from(c->cfg, "otattr");
  if (!m.poolLo && !m.poolHi) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      lucent::warn("otattr", "GameConfig::packetPoolBase/Stride are 0 for this game — packet-pool "
                             "attribution is STRUCTURALLY BLIND here, so an empty span table means "
                             "'not measured', NOT 'the guest submitted nothing'. RE the pool and fill "
                             "those fields to turn this on.");
    }
    return { 0, 0, false };
  }
  return { m.poolLo, m.poolHi, true };
}
}  // namespace

void OtAttr::resetIfNewFrame(uint32_t frame) {
  if (frame == mFrame) return;
  mFrame = frame;
  mSpanCount = 0;
  mSpanOverflow = 0;
}

void OtAttr::trackStoreSlow(Core* c, uint32_t addr, uint32_t bytes) {
  // Reached from Core::mem_w32 via pkt_track — on EVERY guest memory write, the hottest path in the
  // substrate. Looking the channel up by NAME here is not a cheap flag test: it hashes the name under
  // a mutex. MEASURED on the Spyro port with a 6-sample stack profile: 6/6 samples landed in
  // lucent::detail::channel_enabled reached from exactly here.
  //
  // The gate is now the inline `if (!g_otattr_channel)` in ot_attr.h and there is nothing left to
  // re-check here: a Channel re-resolves itself whenever the enabled set changes, so `debug otattr`
  // at the REPL still takes effect on the next store. Anything reaching this function is armed.
  const uint32_t frame  = c->game->gpu.s_frame;
  const uint32_t fn     = c->idiag.otattrTop();
  const uint32_t caller = c->idiag.otattrCaller();
  // Physical form: mask off the segment bits (KUSEG/KSEG0/KSEG1 mirror main RAM at 0x000xxxxx/0x800xxxxx/
  // 0xA00xxxxx — masking with 0x1FFFFFFF collapses all three to the same physical offset) — this is the
  // SAME normalization display_pass_write_guard (mem.cpp) uses, and unlike the pool-only `k = addr |
  // 0x80000000u` below, it also gives scratchpad (0x1F800000-0x1F8003FF) its own correct, un-mangled
  // address instead of folding it into a bogus main-RAM address. Needed here because watched regions
  // (LAST-WRITER PROVENANCE, ot_attr.h) can cover scratchpad, not just the packet pool.
  const uint32_t phys   = addr & 0x1FFFFFFFu;

  recordFnStat(frame, fn, phys);          // fed unconditionally — `otattr trace`'s copy-loop heuristic
  trackWatch(fn, caller, phys, bytes, frame);

  const uint32_t k = addr | 0x80000000u;
  const PoolRange pool = pool_range(c);
  if (!pool.known || k < pool.lo || k >= pool.hi) return;

  resetIfNewFrame(frame);
  // Same node fallback the native GT3/GT4 submit path itself uses (render_internal.h cur_render_node):
  // the walk's beginObject() node when set, else the guest "current render object" scratchpad
  // (0x1F80028C) — most native per-object quad submission (submit.cpp) never opens a diag walk scope,
  // it relies on this scratchpad, so reading raw diag.currentNode() alone would show node=0 for the
  // MAJORITY of world-object quads (the exact case bug #45 cares about).
  const uint32_t node   = cur_render_node(c);

  // Coalesce a run of stores sharing the same attribution into one growing span — a quad's
  // header/vertex/color words collapse to a single entry instead of one per store, which is what
  // keeps SPAN_CAP from blowing out on a normal frame.
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
  if (!g_otattr_channel) return;
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

// --- LAST-WRITER PROVENANCE (watched regions) ---

int OtAttr::watchRegister(uint32_t addr, uint32_t len) {
  if (mWatchCount >= WATCH_SLOTS) { mWatchOverflow++; return -1; }
  const uint32_t phys  = addr & 0x1FFFFFFFu;
  const uint32_t words = (len + 3) / 4;
  if (mWatchWordsUsed + (int)words > WATCH_CAP_WORDS) { mWatchOverflow++; return -1; }

  WatchRegion& r = mWatch[mWatchCount];
  r.lo = phys; r.hi = phys + len; r.wordBase = (uint32_t)mWatchWordsUsed; r.active = true;
  // Fresh region -> its slice of the pooled backing store starts as "never written" (default WordRec has
  // frame=0xFFFFFFFF); no explicit clear needed since a slice is never reused across two live regions.
  mWatchWordsUsed += (int)words;
  return mWatchCount++;
}

void OtAttr::trackWatch(uint32_t fn, uint32_t caller, uint32_t phys, uint32_t bytes, uint32_t frame) {
  if (mWatchCount == 0) return;
  // A store can straddle a word boundary (unaligned byte/half store, or a >4-byte call site) — mark
  // every word touched, not just the first.
  const uint32_t first = phys & ~3u;
  const uint32_t last  = (phys + bytes - 1) & ~3u;
  for (uint32_t w = first; w <= last; w += 4) {
    for (int i = 0; i < mWatchCount; i++) {
      const WatchRegion& r = mWatch[i];
      if (!r.active || w < r.lo || w >= r.hi) continue;
      const uint32_t idx = r.wordBase + (w - r.lo) / 4;
      mWatchWords[idx] = WordRec{ fn, caller, frame };
      break;   // regions don't overlap by construction (each registration carves a fresh slice)
    }
  }
}

bool OtAttr::watchLookup(uint32_t addr, WordRec* out, uint32_t* wordAddrOut) const {
  const uint32_t phys = addr & 0x1FFFFFFFu;
  const uint32_t w = phys & ~3u;
  for (int i = 0; i < mWatchCount; i++) {
    const WatchRegion& r = mWatch[i];
    if (!r.active || w < r.lo || w >= r.hi) continue;
    const uint32_t idx = r.wordBase + (w - r.lo) / 4;
    if (out) *out = mWatchWords[idx];
    if (wordAddrOut) *wordAddrOut = w;
    return true;
  }
  return false;
}

// --- per-fn store-count stat (feeds `otattr trace`'s copy-loop heuristic) ---

void OtAttr::recordFnStat(uint32_t frame, uint32_t fn, uint32_t phys) {
  if (frame != mFnStatFrame) { mFnStatFrame = frame; mFnStatCount = 0; mFnStatOverflow = 0; }
  const uint32_t page = phys & ~0xFFFu;   // 4KB page granularity — coarse enough that a real copy loop
                                          // (fans out across many small structs) blows past FNSTAT_PAGES
                                          // fast, while one object's own quad submission stays within 1-2.
  FnStoreStat* e = nullptr;
  for (int i = 0; i < mFnStatCount; i++) if (mFnStat[i].fn == fn) { e = &mFnStat[i]; break; }
  if (!e) {
    if (mFnStatCount >= FNSTAT_CAP) { mFnStatOverflow++; return; }
    e = &mFnStat[mFnStatCount++];
    *e = FnStoreStat{};
    e->fn = fn;
  }
  e->count++;
  bool haveIt = false;
  for (int i = 0; i < e->pageCount; i++) if (e->pages[i] == page) { haveIt = true; break; }
  if (!haveIt) {
    if (e->pageCount < FNSTAT_PAGES) e->pages[e->pageCount++] = page;
    else e->pageOverflow = true;
  }
}

const OtAttr::FnStoreStat* OtAttr::fnStatFind(uint32_t fn) const {
  for (int i = 0; i < mFnStatCount; i++) if (mFnStat[i].fn == fn) return &mFnStat[i];
  return nullptr;
}
