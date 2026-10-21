// OtAttr implementation — see ot_attr.h for the design. Separated from the header so the header can
// stay a forward-declared-Core, dependency-light include.
#include "ot_attr.h"
#include "core.h"
// NO "game.h" HERE, DELIBERATELY, and it is a compile-enforced fence rather than a convention: with
// `Game` left incomplete (core.h only forward-declares it) any future `c->game->…` in this TU fails to
// build. That is what keeps the store path off the whole machine — see OtAttr::stampFrame for the crash
// that reaching through `c->game` for a frame counter caused.
#include "command_args.h"
#include "native_dispatch.h"
#include "ordering_table.h"
#include "render_substrate.h"
#include <algorithm>
#include <cstdio>
#include <format>
#include <lucent/log.h>
#include <optional>
#include <string_view>
#include <vector>

void OtAttr::runCommand(Core &core, const psx::debug::CommandArgs &args, const Emit &emit) {
  const std::string_view sub = args.word(1);
  if (sub == "watch") {
    watchCommand(args, emit);
  } else if (sub == "who") {
    whoCommand(args, emit);
  } else if (sub == "trace") {
    traceCommand(args, emit);
  } else if (sub == "packets") {
    packetsCommand(core, emit);
  } else if (const std::optional<uint32_t> address = args.hex(1)) {
    attributeOne(*address, emit);
  } else {
    attributeTable(core, emit);
  }
}

// Answers "who wrote this word" for a staging buffer, where call-path attribution only names the batcher.
void OtAttr::watchCommand(const psx::debug::CommandArgs &args, const Emit &emit) {
  const uint32_t addr = args.hex(2).value_or(0);
  const uint32_t len = args.hex(3).value_or(0);
  if (!args.hex(2).has_value() || len == 0) {
    emit(std::format("usage: otattr watch <addr-hex> <len-hex>"));
  } else {
    int slot = watchRegister({addr, len});
    if (slot < 0) {
      emit(std::format("watch REJECTED (slots={}/{} wordsUsed={}/{} overflow={}) — free a slot or shrink the region",
                       watchSlotCount(),
                       (int)OtAttr::WATCH_SLOTS,
                       watchWordsUsed(),
                       (int)OtAttr::WATCH_CAP_WORDS,
                       watchOverflow()));
    } else {
      const OtAttr::WatchRegion *r = watchAt(slot);
      // KSEG0 only for main RAM; the scratchpad prints as itself.
      uint32_t dlo = r->lo < 0x200000u ? (0x80000000u | r->lo) : r->lo;
      uint32_t dhi = r->hi < 0x200000u ? (0x80000000u | r->hi) : r->hi;
      emit(std::format("watch[{}] = [0x{:08X},0x{:08X}) ({} words) — slots {}/{}, {}/{} words used",
                       slot,
                       dlo,
                       dhi,
                       (r->hi - r->lo) / 4,
                       watchSlotCount(),
                       (int)OtAttr::WATCH_SLOTS,
                       watchWordsUsed(),
                       (int)OtAttr::WATCH_CAP_WORDS));
    }
  }
}

void OtAttr::whoCommand(const psx::debug::CommandArgs &args, const Emit &emit) const {
  const std::optional<uint32_t> addrArg = args.hex(2);
  if (!addrArg.has_value()) {
    emit(std::format("usage: otattr who <addr-hex> [len-hex]"));
    return;
  }
  const uint32_t addr = *addrArg;
  const uint32_t len = args.hex(3).value_or(4);
  emit(std::format("who 0x{:08X}..0x{:08X} (word-granular, coalesced runs):", addr, addr + len));
  uint32_t w = addr & ~3u, end = addr + len;
  bool any = false;
  OtAttr::WordRec cur{};
  uint32_t runLo = 0, runHi = 0;
  bool haveRun = false;
  auto flush_run = [&]() {
    if (!haveRun) {
      return;
    }
    if (cur.frame == 0xFFFFFFFFu) {
      emit(std::format("  [0x{:08X},0x{:08X}) NEVER WRITTEN since watch registered", runLo, runHi));
    } else {
      emit(std::format(
          "  [0x{:08X},0x{:08X}) fn=0x{:08X} caller=0x{:08X} frame={}", runLo, runHi, cur.fn, cur.caller, cur.frame));
    }
    any = true;
    haveRun = false;
  };
  for (; w < end; w += 4) {
    OtAttr::WordRec rec{};
    uint32_t wa = 0;
    if (!watchLookup(w, &rec, &wa)) {
      flush_run();
      emit(std::format("  [0x{:08X},0x{:08X}) NOT WATCHED — run `otattr watch` first", w, w + 4));
      return;
    }
    if (haveRun && rec.fn == cur.fn && rec.caller == cur.caller && rec.frame == cur.frame && wa == runHi) {
      runHi = wa + 4;
    } else {
      flush_run();
      cur = rec;
      runLo = wa;
      runHi = wa + 4;
      haveRun = true;
    }
  }
  flush_run();
  if (!any) {
    emit(std::format("  (nothing in range)"));
  }
}

void OtAttr::traceCommand(const psx::debug::CommandArgs &args, const Emit &emit) const {
  const std::optional<uint32_t> traced = args.hex(2);
  if (!traced.has_value()) {
    emit("usage: otattr trace <addr-hex>");
    return;
  }
  const uint32_t addr = *traced;
  WordRec rec{};
  uint32_t wa = 0;
  if (!watchLookup(addr, &rec, &wa)) {
    emit(std::format("trace 0x{:08X}: NOT in any watched region — `otattr watch <addr> <len>` first", addr));
    return;
  }
  if (rec.frame == 0xFFFFFFFFu) {
    emit(std::format("trace 0x{:08X}: watched, but NEVER WRITTEN since registration", addr));
    return;
  }
  emit(std::format("trace 0x{:08X} (word 0x{:08X}): last writer fn=0x{:08X} caller=0x{:08X} frame={}",
                   addr,
                   wa,
                   rec.fn,
                   rec.caller,
                   rec.frame));
  // A writer that touches many 4 KB pages in one frame is a copy loop: the object identity was lost before it.
  const FnStoreStat *st = fnStatFind(rec.fn);
  if (st == nullptr) {
    emit("  no per-fn store stat for this fn this frame; trace in the frame the write happened");
    return;
  }
  const bool copyLoop = st->pageOverflow || st->pageCount >= 3;
  emit(std::format("  fn=0x{:08X} made {} store(s) over {} distinct 4KB page(s){} -> {}",
                   rec.fn,
                   st->count,
                   st->pageCount,
                   st->pageOverflow ? "+" : "",
                   copyLoop ? "a copy/batch loop: decompile it for its source, then `otattr watch` that"
                            : "a direct, single-destination writer"));
}

void OtAttr::packetsCommand(Core &core, const Emit &emit) const {
  if (core.rsub.lastOrderingTable == 0) {
    emit("no ordering table walked yet this run");
    return;
  }
  psx::gpu::OrderingTableCursor cursor(core, core.rsub.lastOrderingTable);
  do {
    const psx::gpu::OtNode &node = cursor.node();
    if (!node.carriesPrimitive()) {
      continue;
    }
    const uint32_t command = core.mem_r32(node.gp0WordRamOffset(0));
    const uint32_t vertex = node.gp0WordCount() >= 2 ? core.mem_r32(node.gp0WordRamOffset(1)) : 0u;
    Span sp{};
    const bool found = lookupStore(node.gp0WordRamOffset(0), &sp);
    emit(std::format("  [{:4}] packet=0x{:08X} op=0x{:02X} n={} v0=({},{}) fn=0x{:08X} caller=0x{:08X}",
                     cursor.nodesEntered() - 1,
                     node.guestAddress(),
                     command >> 24,
                     node.gp0WordCount(),
                     static_cast<int16_t>(vertex & 0xFFFFu),
                     static_cast<int16_t>(vertex >> 16),
                     found ? sp.fn : 0u,
                     found ? sp.caller : 0u));
  } while (cursor.advance());
  emit(
      std::format("GTE RTPS/RTPT calls this frame by fn ({} fn(s){}):", mGteCount, mGteOverflow ? ", overflowed" : ""));
  for (int i = 0; i < mGteCount; i++) {
    emit(std::format("  fn=0x{:08X} count={}", mGte[i].fn, mGte[i].count));
  }
}

void OtAttr::attributeOne(uint32_t address, const Emit &emit) const {
  Span sp{};
  if (lookupStore(address, &sp)) {
    emit(std::format("0x{:08X} <- fn=0x{:08X} caller=0x{:08X} claimed=0x{:08X} (span [0x{:08X},0x{:08X}))",
                     address | 0x80000000u,
                     sp.fn,
                     sp.caller,
                     sp.claimed,
                     sp.lo,
                     sp.hi));
    return;
  }
  // No span is "not recorded", not "nobody wrote it".
  emit(std::format("0x{:08X}: no span covers it ({} spans this frame{})",
                   address | 0x80000000u,
                   mSpanCount,
                   mSpanOverflow ? ", the table overflowed" : ""));
}

// Attribution is not total: a packet stored before the table was armed, or past its cap, is unattributed.
void OtAttr::attributeTable(Core &core, const Emit &emit) const {
  struct Row {
    uint32_t fn;
    int packets;
  };
  std::vector<Row> rows;
  int nodes = 0;
  int attributed = 0;
  if (core.rsub.lastOrderingTable != 0) {
    psx::gpu::OrderingTableCursor cursor(core, core.rsub.lastOrderingTable);
    do {
      if (!cursor.node().carriesPrimitive()) {
        continue;
      }
      nodes++;
      Span sp{};
      if (!lookupStore(cursor.node().gp0WordRamOffset(0), &sp)) {
        continue;
      }
      attributed++;
      const auto row = std::find_if(rows.begin(), rows.end(), [&sp](const Row &r) {
        return r.fn == sp.fn;
      });
      if (row != rows.end()) {
        row->packets++;
      } else {
        rows.push_back({sp.fn, 1});
      }
    } while (cursor.advance());
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    return a.packets > b.packets;
  });
  emit(std::format("frame {} OT@0x{:08X}: {} drawing nodes, {} attributed, {} UNATTRIBUTED ({} spans{}); watch "
                   "slots {}/{} words {}/{}",
                   mFrame,
                   0x80000000u | core.rsub.lastOrderingTable,
                   nodes,
                   attributed,
                   nodes - attributed,
                   mSpanCount,
                   mSpanOverflow ? ", TABLE OVERFLOWED" : "",
                   watchSlotCount(),
                   static_cast<int>(WATCH_SLOTS),
                   watchWordsUsed(),
                   static_cast<int>(WATCH_CAP_WORDS)));
  for (const Row &row : rows) {
    emit(std::format("  fn=0x{:08X}  {} packet(s)", row.fn, row.packets));
  }
}

void OtAttr::refreshPool(Core *c) {
  mPoolStale = false;
  const std::optional<psx::gpu::PacketPoolExtent> pool = c->runtime.packetPool(*c);
  if (pool) {
    mPool = *pool;
    return;
  }
  mPool = {};
  if (mWarnedNoPool) {
    return;
  }
  mWarnedNoPool = true;
  lucent::warn("otattr", "the title declares no packet pool, so packet attribution and GuestPacketFilter see nothing");
}

// The frame stamp every table here shares — see ot_attr.h for the null-deref this replaced and for why
// there is now exactly ONE clock instead of a logic clock for the spans and a present clock for the rest.
uint32_t OtAttr::stampFrame() {
  if (mFrame != NO_FRAME) {
    return mFrame;
  }
  // Pre-loop boot stores are normal in a game run. Count them and use frame 0, but defer the verdict
  // until run end: only then can we distinguish "the loop has not started YET" from "no loop ever
  // declared a frame". The old immediate warning accused every healthy port during crt0.
  mPreFrameStamps++;
  return 0;
}

void OtAttr::reportFrameContract(const char *context) const {
  const char *who = (context && *context) ? context : "run";
  if (mFrame != NO_FRAME) {
    lucent::info("otattr",
                 "{}: frame-loop contract SATISFIED — beginLogicFrame reached frame {} "
                 "after {} pre-frame stamp(s)",
                 who,
                 mFrame,
                 mPreFrameStamps);
    return;
  }
  if (mPreFrameStamps == 0) {
    lucent::info("otattr",
                 "{}: frame-loop contract NOT EXERCISED — 0 table stamps and 0 declared "
                 "frames; there is no per-frame data to certify",
                 who);
    return;
  }
  lucent::warn("otattr",
               "{}: frame-loop contract FAILED — {} table stamp(s) occurred but nothing "
               "called OtAttr::beginLogicFrame. The span / watch / per-fn tables were "
               "stamped frame 0 and never reset; their contents span the whole run, not "
               "one frame.",
               who,
               mPreFrameStamps);
}

void OtAttr::resetIfNewFrame(uint32_t frame) {
  if (frame == mFrame) {
    return;
  }
  mFrame = frame;
  mSpanCount = 0;
  mSpanOverflow = 0;
  mSpansSorted = true; // a fresh frame starts sorted by construction; trackStore un-sets it if not
}

void OtAttr::trackStoreSlow(Core *c, GuestSpan store) {
  const uint32_t addr = store.address;
  const uint32_t bytes = store.bytes;
  // Reached from Core::mem_w32 via pkt_track — on EVERY guest memory write, the hottest path in the
  // substrate. Looking the channel up by NAME here is not a cheap flag test: it hashes the name under
  // a mutex. MEASURED on the Spyro port with a 6-sample stack profile: 6/6 samples landed in
  // lucent::detail::channel_enabled reached from exactly here.
  //
  // The gate is now the inline `if (!g_otattr_channel)` in ot_attr.h and there is nothing left to
  // re-check here: a Channel re-resolves itself whenever the enabled set changes, so `debug otattr`
  // at the REPL still takes effect on the next store. Anything reaching this function is armed.
  const uint32_t frame = stampFrame();
  const uint32_t fn = c->callAttribution.top();
  const uint32_t caller = c->callAttribution.caller();
  // Physical form: mask off the segment bits (KUSEG/KSEG0/KSEG1 mirror main RAM at 0x000xxxxx/0x800xxxxx/
  // 0xA00xxxxx — masking with 0x1FFFFFFF collapses all three to the same physical offset) — this is the
  // SAME normalization display_pass_write_guard (mem.cpp) uses, and unlike the pool-only `k = addr |
  // 0x80000000u` below, it also gives scratchpad (0x1F800000-0x1F8003FF) its own correct, un-mangled
  // address instead of folding it into a bogus main-RAM address. Needed here because watched regions
  // (LAST-WRITER PROVENANCE, ot_attr.h) can cover scratchpad, not just the packet pool.
  const uint32_t phys = addr & 0x1FFFFFFFu;

  // GATED ON THE CHANNEL, and the gate is LOAD-BEARING — shipping the direct-call push without it costs
  // +24% user CPU on pc_render and +87% on psx_render (1200-frame replay, PSXPORT_NOPACE=1, measured).
  // recordFnStat LINEAR-SCANS up to FNSTAT_CAP entries keyed on `fn`, on EVERY guest store, before the
  // pool-range early-out. That was O(1) BY ACCIDENT while otattrTop() was structurally 0: one row, always
  // a first-iteration hit. Maintaining the stack on direct calls turns it into a real scan. It is read
  // ONLY by `otattr trace`, whose REPL path already reports "no per-fn store stat recorded" when empty.
  if (g_otattr_channel) {
    recordFnStat({fn, caller, frame}, phys);
  }
  trackWatch({fn, caller, frame}, {phys, bytes});

  const uint32_t k = phys | 0x80000000u;
  if (mPoolStale) {
    refreshPool(c);
  }
  // Core calls this before committing the store, so the next store re-reads the rewritten bound.
  if (mPool.readFrom({k, bytes})) {
    mPoolStale = true;
    return;
  }
  if (!mPool.overlaps({k, bytes})) {
    return;
  }

  // Coalesce a run of stores sharing the same attribution into one growing span — a quad's
  // header/vertex/color words collapse to a single entry instead of one per store, which is what
  // keeps SPAN_CAP from blowing out on a normal frame.
  if (mSpanCount > 0) {
    Span &last = mSpans[mSpanCount - 1];
    if (last.fn == fn && last.caller == caller && k >= last.lo && k <= last.hi) {
      if (k + bytes > last.hi) {
        last.hi = k + bytes;
      }
      return;
    }
  }
  // Resolved HERE and not at GP0-execution time, because the call chain only exists during the store —
  // by the time the packet is executed the guest has long returned and there is nothing left to walk.
  const uint32_t claimed = resolveClaimedFrame(c);
  if (mSpanCount < SPAN_CAP) {
    // Track whether the table stayed ADDRESS-SORTED. The pool pointer is monotonic within a frame (the
    // reason the reverse linear scan below is safe), so in practice it is — and that is what lets
    // lookupStore binary-search. Checked rather than assumed: the fallback pass looks a span up per OT
    // node, so a linear scan there is O(nodes x spans) and measured at ~4x the frame cost.
    if (mSpanCount > 0 && k < mSpans[mSpanCount - 1].lo) {
      mSpansSorted = false;
    }
    mSpans[mSpanCount++] = Span{k, k + bytes, fn, caller, c->pc, claimed};
  } else {
    mSpanOverflow++;
  }
}

uint32_t OtAttr::resolveClaimedFrame(Core *c) {
  const psx::cpu::NativeDispatcher &dispatcher = c->nativeDispatcher();
  if (!dispatcher.hasProducers()) {
    return 0;
  }
  if (mResolveCacheRevision != dispatcher.producerRevision()) {
    for (int i = 0; i < RCACHE_SLOTS; i++) {
      mResolveCache[i].depth = -1;
    }
    mResolveCacheRevision = dispatcher.producerRevision();
  }
  const int visNow = c->callAttribution.visibleDepth();
  const int limNow = visNow < CLAIM_SEARCH_DEPTH ? visNow : CLAIM_SEARCH_DEPTH;
  uint32_t prefix[CLAIM_SEARCH_DEPTH] = {};
  uint32_t h = 2166136261u; // FNV-1a over the prefix
  for (int i = 0; i < limNow; i++) {
    prefix[i] = c->callAttribution.frameFromTop(i);
    h = (h ^ prefix[i]) * 16777619u;
  }
  ResolveEntry &slot = mResolveCache[(h ^ (uint32_t)limNow) & (RCACHE_SLOTS - 1)];
  if (slot.depth == limNow) {
    bool same = true;
    for (int i = 0; i < limNow && same; i++) {
      same = slot.prefix[i] == prefix[i];
    }
    if (same) {
      return slot.result;
    }
  }
  uint32_t found = 0;
  for (int i = 0; i < limNow; i++) {
    if (prefix[i] && dispatcher.isProducer(prefix[i])) {
      found = prefix[i];
      break;
    }
  }
  slot.depth = limNow;
  for (int i = 0; i < limNow; i++) {
    slot.prefix[i] = prefix[i];
  }
  slot.result = found;
  return found;
}

psx::debug::ProducerAttribution OtAttr::submitterOf(Core *c, uint32_t packetAddress) const {
  if (packetAddress == 0) {
    return {0, psx::debug::Unattributed::NoSource};
  }
  Span span{};
  if (!lookupStore(packetAddress & 0x1FFFFCu, &span)) {
    return {0, psx::debug::Unattributed::SpanMiss};
  }
  if (span.claimed) {
    return {span.claimed, psx::debug::Unattributed::NoSource};
  }
  if (span.fn) {
    return {span.fn, psx::debug::Unattributed::NoSource};
  }
  return {0, psx::debug::Unattributed::SpanNoFn};
}

void OtAttr::trackGte(Core *c) {
  if (!g_otattr_channel) {
    return;
  }
  // Same one clock as the store path, and for the same reason: Gte::execute is a core primitive too, so
  // reaching through `c->game` for a frame counter would crash a Core-alone embedder that executes GTE.
  const uint32_t frame = stampFrame();
  if (frame != mGteFrame) {
    mGteFrame = frame;
    mGteCount = 0;
    mGteOverflow = 0;
  }

  const uint32_t fn = c->callAttribution.top();
  for (int i = 0; i < mGteCount; i++) {
    if (mGte[i].fn == fn) {
      mGte[i].count++;
      return;
    }
  }
  if (mGteCount < GTE_CAP) {
    mGte[mGteCount++] = GteBucket{fn, 1};
  } else {
    mGteOverflow++;
  }
}

bool OtAttr::lookupStore(uint32_t addr, Span *out) const {
  const uint32_t k = addr | 0x80000000u;
  // BINARY SEARCH when the table is address-sorted, which it is whenever the pool pointer only moved
  // forward this frame (trackStore checks, it does not assume). This is not a micro-optimisation: the
  // unclaimed-geometry fallback asks once per OT node, so the reverse scan below made the pass
  // O(nodes x spans) — measured at ~29 fps against ~110 without it.
  if (mSpansSorted && mSpanCount > 8) {
    int lo = 0, hi = mSpanCount - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      if (k < mSpans[mid].lo) {
        hi = mid - 1;
      } else if (k >= mSpans[mid].hi) {
        lo = mid + 1;
      } else {
        if (out) {
          *out = mSpans[mid];
        }
        return true;
      }
    }
    return false;
  }
  // Most-recent-first: within one frame the pool pointer is monotonic, so addresses aren't reused, but
  // scanning backward costs nothing extra and is the more useful order if that ever changes.
  for (int i = mSpanCount - 1; i >= 0; i--) {
    if (k >= mSpans[i].lo && k < mSpans[i].hi) {
      if (out) {
        *out = mSpans[i];
      }
      return true;
    }
  }
  return false;
}

// --- LAST-WRITER PROVENANCE (watched regions) ---

int OtAttr::watchRegister(GuestSpan region) {
  const uint32_t addr = region.address;
  const uint32_t len = region.bytes;
  if (mWatchCount >= WATCH_SLOTS) {
    mWatchOverflow++;
    return -1;
  }
  const uint32_t phys = addr & 0x1FFFFFFFu;
  const uint32_t words = (len + 3) / 4;
  if (mWatchWordsUsed + (int)words > WATCH_CAP_WORDS) {
    mWatchOverflow++;
    return -1;
  }

  WatchRegion &r = mWatch[mWatchCount];
  r.lo = phys;
  r.hi = phys + len;
  r.wordBase = (uint32_t)mWatchWordsUsed;
  r.active = true;
  // Fresh region -> its slice of the pooled backing store starts as "never written" (default WordRec has
  // frame=0xFFFFFFFF); no explicit clear needed since a slice is never reused across two live regions.
  mWatchWordsUsed += (int)words;
  return mWatchCount++;
}

void OtAttr::trackWatch(StoreOrigin origin, GuestSpan physical) {
  const uint32_t phys = physical.address;
  const uint32_t bytes = physical.bytes;
  if (mWatchCount == 0) {
    return;
  }
  // A store can straddle a word boundary (unaligned byte/half store, or a >4-byte call site) — mark
  // every word touched, not just the first.
  const uint32_t first = phys & ~3u;
  const uint32_t last = (phys + bytes - 1) & ~3u;
  for (uint32_t w = first; w <= last; w += 4) {
    for (int i = 0; i < mWatchCount; i++) {
      const WatchRegion &r = mWatch[i];
      if (!r.active || w < r.lo || w >= r.hi) {
        continue;
      }
      const uint32_t idx = r.wordBase + (w - r.lo) / 4;
      mWatchWords[idx] = WordRec{origin.fn, origin.caller, origin.frame};
      break; // regions don't overlap by construction (each registration carves a fresh slice)
    }
  }
}

bool OtAttr::watchLookup(uint32_t addr, WordRec *out, uint32_t *wordAddrOut) const {
  const uint32_t phys = addr & 0x1FFFFFFFu;
  const uint32_t w = phys & ~3u;
  for (int i = 0; i < mWatchCount; i++) {
    const WatchRegion &r = mWatch[i];
    if (!r.active || w < r.lo || w >= r.hi) {
      continue;
    }
    const uint32_t idx = r.wordBase + (w - r.lo) / 4;
    if (out) {
      *out = mWatchWords[idx];
    }
    if (wordAddrOut) {
      *wordAddrOut = w;
    }
    return true;
  }
  return false;
}

// --- per-fn store-count stat (feeds `otattr trace`'s copy-loop heuristic) ---

void OtAttr::recordFnStat(StoreOrigin origin, uint32_t phys) {
  const uint32_t frame = origin.frame;
  const uint32_t fn = origin.fn;
  if (frame != mFnStatFrame) {
    mFnStatFrame = frame;
    mFnStatCount = 0;
    mFnStatOverflow = 0;
  }
  const uint32_t page = phys & ~0xFFFu; // 4KB page granularity — coarse enough that a real copy loop
                                        // (fans out across many small structs) blows past FNSTAT_PAGES
                                        // fast, while one object's own quad submission stays within 1-2.
  FnStoreStat *e = nullptr;
  for (int i = 0; i < mFnStatCount; i++) {
    if (mFnStat[i].fn == fn) {
      e = &mFnStat[i];
      break;
    }
  }
  if (!e) {
    if (mFnStatCount >= FNSTAT_CAP) {
      mFnStatOverflow++;
      return;
    }
    e = &mFnStat[mFnStatCount++];
    *e = FnStoreStat{};
    e->fn = fn;
  }
  e->count++;
  bool haveIt = false;
  for (int i = 0; i < e->pageCount; i++) {
    if (e->pages[i] == page) {
      haveIt = true;
      break;
    }
  }
  if (!haveIt) {
    if (e->pageCount < FNSTAT_PAGES) {
      e->pages[e->pageCount++] = page;
    } else {
      e->pageOverflow = true;
    }
  }
}

const OtAttr::FnStoreStat *OtAttr::fnStatFind(uint32_t fn) const {
  for (int i = 0; i < mFnStatCount; i++) {
    if (mFnStat[i].fn == fn) {
      return &mFnStat[i];
    }
  }
  return nullptr;
}
