// OtAttr implementation — see ot_attr.h for the design. Separated from the header so the header can
// stay a forward-declared-Core, dependency-light include.
#include "ot_attr.h"
#include "render_node.h"   // cur_render_node — same node fallback the native submit path itself uses
#include "core.h"
#include "game.h"
#include "cfg.h"

// Packet pool range (RE'd render-buffer map): the shared pool every GP0 packet submitter
// (owned native or still-substrate) writes into.
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
  if (k < POOL_LO || k >= POOL_HI) return;

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
