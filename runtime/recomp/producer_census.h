// producer_census.h — class ProducerCensus: WHO DREW THIS FRAME, per producer, per Core.
//
// THE ASK (USER, 2026-08-11): *"I need a DB for each game to keep track of native graphics producers,
// framework should do this automatically, when the game renders an effect, if it's not in the DB then a
// DB entry gets created and we need to track if it has a native producer equivalent"* — populated by
// ordinary play, *"then I can tell you something like 'work on the DB entries'"*. Full design, schema
// and staging: docs/plans/graphics-producer-db.md.
//
// THE NUMBER THIS EXISTS TO PRODUCE, and why nothing else in the workspace can: three registries
// already say what code EXISTS (codemap), what has been TRIED (issue-catalog) and which RE steps are
// REAL (re-frontier), and `overrides::coverage` says how much of what we OWN a run reached — a fraction
// whose denominator is our own ambition. The missing fraction is the opposite one: of the
// picture-producing work the game DID this run, how much does the PC produce natively? That denominator
// is discovered by RUNNING, and this is the table that discovers it.
//
// WHAT IT IS NOT: an ordering, a renderer, or a diagnostic channel. It is a per-Core value table with
// no Core pointer, no GPU dependency and no I/O, so it is testable on its inputs alone
// (tests/test_producer_census.cpp) and cheap enough to leave ON in a normal windowed run — which the ask
// requires, because the user's own play session is the primary data source.
//
// DESIGNED-NEGATIVE FIRST (global CLAUDE.md: "a diagnostic that can print nothing is lying"). Every
// count that a report could read as "nothing to see" is separated from "I never looked":
//   wasFed()         — was this census fed AT ALL? An unfed census and a clean one are different runs.
//   primsSeen()      — the denominator: prims the census was told about, attributable or not.
//   gp0Anon()        — prims with no GP0 source address: PERMANENTLY unattributable, not unowned.
//   spanMiss()       — prims whose packet address matched no attribution span: the feed was off or
//                      overflowed. Not "this prim has no producer".
//   unscopedNative() — native pushes with no ProducerScope open: real work by an UNDECLARED producer.
//   overflow()       — producers that did not fit the table, so a short list cannot read as complete.
// A report that prints a row list without these six is not allowed to claim coverage.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <lucent/log.h>

// ---- ProducerKey — the row identity -------------------------------------------------------------
//
// DECIDED with the user (2026-08-11), with the alternatives on the table (fn + material signature;
// named-effect rows with fns as attributes): **the row IS the guest submitter fn address**, created
// mechanically on first sight, with the name and RE status curated on it afterwards.
//
// Why this key: it lives in the SAME space as `overrides::install`'s key, which is what makes "does
// this effect have a native producer" a DERIVED fact rather than a box someone has to remember to tick.
// A name cannot be minted at run time, so a name-keyed table cannot satisfy "an entry gets created"; a
// material signature splits one effect across texpages and merges unrelated ones.
//
// `native` keys exist for PC-only producers (an enhancement with no guest counterpart). They are a
// SEPARATE space from guest addresses — `native(7)` and `guest(7)` are different rows — because an
// interned producer id and a guest address are different kinds of thing and collapsing them would
// silently merge two producers.
struct ProducerKey {
  enum Kind : uint8_t { NONE = 0, GUEST = 1, NATIVE_ONLY = 2 };
  Kind     kind = NONE;
  uint32_t id   = 0;      // GUEST: the guest fn address. NATIVE_ONLY: an interned producer id.

  static ProducerKey guest(uint32_t addr)  { return ProducerKey{GUEST, addr}; }
  static ProducerKey native(uint32_t iid)  { return ProducerKey{NATIVE_ONLY, iid}; }
  // NONE is not "unknown, pick something" — it is the explicit statement that the caller could not
  // name a producer, and it routes to unscopedNative()/spanMiss() rather than to a row.
  static ProducerKey none()                { return ProducerKey{NONE, 0}; }

  bool valid()        const { return kind != NONE; }
  bool isNativeOnly() const { return kind == NATIVE_ONLY; }
  bool operator==(const ProducerKey& o) const { return kind == o.kind && id == o.id; }
};

class ProducerCensus {
 public:
  // A play session traverses a lot of scenes; Tomba!2 has 68 files under game/render/ and the guest
  // has more producers than that. 1024 rows is generous for one run and still a fixed ~40 KB per Core,
  // and going over is REPORTED (overflow()) rather than dropped silently.
  static constexpr int CAP = 1024;

  // Why a prim could not be attributed. Named rather than bool'd so a report can say WHICH blindness
  // it hit — the two have different fixes (one is unfixable, one means the feed is off).
  enum Why : uint8_t {
    WHY_GP0_ANON   = 0,  // the GP0 words carried no guest source address at all
    WHY_SPAN_MISS  = 1,  // there was an address, but no attribution span covered it
    // A span WAS found and its emitter fn is 0: nothing indirectly-dispatched was on OtAttr's shadow
    // stack when the store happened, so the span cannot name a producer. This is its own reason because
    // its fix differs from the other two — it is not a missing span, it is a span that knows the address
    // and not the author. Keeping it separate is what stops a key of 0 becoming a ROW: measured on the
    // gte leg, 107,837 prims all had fn=0 and collapsed into one bogus row keyed 0x00000000, which is
    // worse than being uncounted because it looks like a producer.
    WHY_SPAN_NO_FN = 2,
  };

  struct Row {
    ProducerKey key;
    uint32_t primsGuest  = 0;   // prims attributed to this producer on the guest/GTE leg
    uint32_t primsNative = 0;   // prims its native ProducerScope pushed
    uint32_t firstFrame  = 0;   // first sighting (never overwritten — it is the "first_seen" the DB keeps)
    uint32_t lastFrame   = 0;
    uint32_t frames      = 0;   // distinct frames it was seen in (a one-frame flash vs a constant draw)
  };

  // ---- feed -------------------------------------------------------------------------------------
  // The guest leg: `addr` is the submitter fn (OtAttr::Span::fn). An invalid key never reaches here —
  // callers that could not attribute a prim call noteUnattributable instead, which is the whole point.
  void noteGuest(uint32_t fnAddr, uint32_t prims, uint32_t frame) {
    note(ProducerKey::guest(fnAddr), prims, frame, /*native=*/false);
  }
  // The native leg. A ProducerKey::none() here means the pushes arrived outside any ProducerScope:
  // counted as unscoped, never attributed to whichever row happened to be last.
  void noteNative(ProducerKey key, uint32_t prims, uint32_t frame) {
    noteNativeLayer(key, prims, frame, /*layer=*/-1);
  }
  // Same, but records WHICH DRAW LAYER an UNSCOPED push came from. Without this the report can only say
  // "1.3M prims were drawn by nobody", which names the size of the problem and not its location — and
  // the obvious way to find the location (guess a producer, scope it, re-run) is exactly the
  // re-derivation this DB exists to replace. The layer is free at the call site, so the report can rank
  // the un-declared work by pass and say which producer family to scope next.
  void noteNativeLayer(ProducerKey key, uint32_t prims, uint32_t frame, int layer) {
    if (!key.valid()) {
      mFed = true;
      mUnscopedNative += prims;
      mPrimsSeen += prims;
      if (layer >= 0 && layer < LAYER_CAP) mUnscopedByLayer[layer] += prims;
      else                                 mUnscopedLayerUnknown += prims;
      return;
    }
    note(key, prims, frame, /*native=*/true);
  }
  // Prims the census SAW and could not attribute. Recording these is what makes a coverage number
  // honest; dropping them is what makes "100% attributed" meaningless.
  void noteUnattributable(Why why, uint32_t prims) {
    mFed = true;
    mPrimsSeen += prims;
    if (why == WHY_GP0_ANON)        mGp0Anon += prims;
    else if (why == WHY_SPAN_NO_FN) mSpanNoFn += prims;
    else                            mSpanMiss += prims;
  }

  // ---- read -------------------------------------------------------------------------------------
  int rowCount() const { return mRowCount; }
  const Row* rowAt(int i) const { return (i >= 0 && i < mRowCount) ? &mRows[i] : nullptr; }
  const Row* find(ProducerKey key) const {
    for (int i = 0; i < mRowCount; i++) if (mRows[i].key == key) return &mRows[i];
    return nullptr;
  }

  bool     wasFed()          const { return mFed; }
  uint64_t primsSeen()       const { return mPrimsSeen; }
  uint64_t primsAttributed() const {
    return mPrimsSeen - mGp0Anon - mSpanMiss - mSpanNoFn - mUnscopedNative;
  }
  uint64_t gp0Anon()         const { return mGp0Anon; }
  uint64_t spanMiss()         const { return mSpanMiss; }
  uint64_t spanNoFn()        const { return mSpanNoFn; }
  uint64_t unscopedNative()  const { return mUnscopedNative; }
  int      overflow()        const { return mOverflow; }
  static constexpr int LAYER_CAP = 8;    // RqLayer today is 0..3; headroom without a dependency on it
  uint64_t unscopedInLayer(int l) const {
    return (l >= 0 && l < LAYER_CAP) ? mUnscopedByLayer[l] : 0;
  }
  uint64_t unscopedLayerUnknown() const { return mUnscopedLayerUnknown; }

  // ---- persistence: one JSONL per run, which is how the DB reaches GIT ---------------------------
  // USER 2026-08-11: the DB "should also be populated when I'm playing" AND "should be in the git".
  // A run therefore appends its rows to scratch/producers/run-<stamp>.jsonl (gitignored), and the
  // game-side tools/producers.py folds those into docs/producers/*.md, which IS tracked. This writer
  // owns the OBSERVED half only; nothing curated is ever emitted here.
  //
  // Field names are producers.py's contract (its _fold reads kind/prims_guest/prims_native/frames/
  // seen_at, and a `{"type":"totals"}` record for the denominators). Changing a name here silently
  // stops rows folding, so they are matched deliberately.
  //
  // A FAILED WRITE IS REPORTED, NOT SWALLOWED: a census that counted a whole session and then quietly
  // failed to persist it is the same silence as never having been fed, and the next run would look like
  // the game simply drew nothing new.
  void writeJsonl(const char* path, const char* stamp) const {
    if (!mFed) {
      lucent::warn("producers",
                   "NOT writing {}: the census was never fed this run, so there is nothing observed to "
                   "persist. This is not an empty run — it is an unwired or unexecuted feed.", path);
      return;
    }
    FILE* f = fopen(path, "ab");
    if (!f) {
      lucent::error("producers",
                    "could not open {} for append — THIS RUN'S OBSERVATIONS ARE LOST ({} row(s), {} "
                    "prims). The directory may not exist; it is created by the caller, not here.",
                    path, mRowCount, (unsigned long long)mPrimsSeen);
      return;
    }
    for (int i = 0; i < mRowCount; i++) {
      const Row& r = mRows[i];
      fprintf(f,
              "{\"key\":\"0x%08X\",\"kind\":\"%s\",\"prims_guest\":%u,\"prims_native\":%u,"
              "\"frames\":%u,\"first_frame\":%u,\"last_frame\":%u,\"seen_at\":\"%s\"}\n",
              r.key.id, r.key.isNativeOnly() ? "native-only" : "guest",
              r.primsGuest, r.primsNative, r.frames, r.firstFrame, r.lastFrame, stamp ? stamp : "");
    }
    // The denominators travel WITH the rows. Without them a later reader can compute a coverage
    // percentage from the rows alone and be confidently wrong, because the rows say nothing about the
    // prims that could not be attributed at all.
    fprintf(f,
            "{\"type\":\"totals\",\"prims_seen\":%llu,\"prims_attributed\":%llu,"
            "\"gp0_anon\":%llu,\"span_miss\":%llu,\"span_no_fn\":%llu,"
            "\"unscoped_native\":%llu,\"overflow\":%d,"
            "\"rows\":%d,\"seen_at\":\"%s\"}\n",
            (unsigned long long)mPrimsSeen, (unsigned long long)primsAttributed(),
            (unsigned long long)mGp0Anon, (unsigned long long)mSpanMiss,
            (unsigned long long)mSpanNoFn,
            (unsigned long long)mUnscopedNative, mOverflow, mRowCount, stamp ? stamp : "");
    fclose(f);
    lucent::info("producers", "wrote {} row(s) + totals -> {}", mRowCount, path);
  }

  // ---- the run's summary line, and it must be readable when it found NOTHING ---------------------
  // A census that was never fed and a census that counted zero prims are DIFFERENT facts with
  // different fixes (the feed is unwired vs no native producer ran), and they print identically unless
  // said outright. Likewise a coverage percentage with no denominator is unreadable, so every blind
  // spot is named with its count rather than folded into "attributed".
  void report(const char* who) const {
    if (!mFed) {
      lucent::warn("producers",
                   "{}: the producer census was NEVER FED this run — 0 notes reached it. This is NOT "
                   "'no producer drew anything'; it means the feed is not wired or never executed, so "
                   "the DB learned NOTHING from this run.", who);
      return;
    }
    lucent::info("producers",
                 "{}: {} row(s); prims seen {} = attributed {} + unscoped-native {} + gp0-anon {} + "
                 "span-miss {} + span-no-fn {}{}",
                 who, mRowCount, (unsigned long long)mPrimsSeen,
                 (unsigned long long)primsAttributed(), (unsigned long long)mUnscopedNative,
                 (unsigned long long)mGp0Anon, (unsigned long long)mSpanMiss,
                 (unsigned long long)mSpanNoFn,
                 mOverflow ? " [ROW TABLE OVERFLOWED — rows were DROPPED]" : "");
    // THE ROWS THEMSELVES, ranked. Totals alone say how much is attributed but not TO WHAT, and "which
    // producers did this run actually see" is the question the DB exists to answer — `producers.py
    // report --todo` will consume the JSONL, but a run should be readable without a second tool.
    {
      int order[CAP];
      const int n = mRowCount;
      for (int i = 0; i < n; i++) order[i] = i;
      for (int i = 1; i < n; i++) {          // insertion sort: n is small and this must not allocate
        const int v = order[i];
        int j = i - 1;
        while (j >= 0 && (mRows[order[j]].primsNative + mRows[order[j]].primsGuest) <
                         (mRows[v].primsNative + mRows[v].primsGuest)) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
      }
      const int shown = n < 16 ? n : 16;
      for (int i = 0; i < shown; i++) {
        const Row& r = mRows[order[i]];
        lucent::info("producers", "  {} 0x{:08X}  native {}  guest {}  frames {} (f{}..f{})",
                     r.key.isNativeOnly() ? "pc-only" : "guest  ", r.key.id,
                     r.primsNative, r.primsGuest, r.frames, r.firstFrame, r.lastFrame);
      }
      if (n > shown)
        lucent::info("producers", "  … {} more row(s) not shown (full set goes to the JSONL writer)",
                     n - shown);
    }
    if (mUnscopedNative) {
      // WHERE the undeclared work is, ranked, so the next producer to scope is measured not guessed.
      // Layer names match RqLayer (render_queue.h) without including it — this header is used by
      // hermetic tests that must not pull the queue in.
      static const char* kLayer[LAYER_CAP] = { "background", "world", "overlay", "hud",
                                               "layer4", "layer5", "layer6", "layer7" };
      lucent::Line ln;
      ln.add("{}: undeclared native prims BY LAYER:", who);
      for (int l = 0; l < LAYER_CAP; l++)
        if (mUnscopedByLayer[l]) ln.add(" {}={}", kLayer[l], (unsigned long long)mUnscopedByLayer[l]);
      if (mUnscopedLayerUnknown) ln.add(" (no-layer-reported={})",
                                        (unsigned long long)mUnscopedLayerUnknown);
      ln.flush(lucent::Level::Warn, "producers");
      lucent::warn("producers",
                   "{}: {} native prim(s) drew with NO ProducerScope open — real work by an UNDECLARED "
                   "producer. Those are missing DB rows, not noise: wrap the producer in a "
                   "ProducerScope naming its guest address.", who, (unsigned long long)mUnscopedNative);
    }
  }

 private:
  void note(ProducerKey key, uint32_t prims, uint32_t frame, bool native) {
    mFed = true;
    mPrimsSeen += prims;
    Row* r = nullptr;
    for (int i = 0; i < mRowCount; i++) if (mRows[i].key == key) { r = &mRows[i]; break; }
    if (!r) {
      if (mRowCount >= CAP) { mOverflow++; return; }   // the prim still counted as seen+attributed
      r = &mRows[mRowCount++];
      r->key = key;
      r->firstFrame = frame;
      r->lastFrame  = frame;
      r->frames     = 1;
    } else if (frame != r->lastFrame) {
      r->lastFrame = frame;
      r->frames++;
    }
    if (native) r->primsNative += prims; else r->primsGuest += prims;
  }

  uint64_t mSpanNoFn = 0;
  uint64_t mUnscopedByLayer[LAYER_CAP] = {};
  uint64_t mUnscopedLayerUnknown = 0;
  Row      mRows[CAP] = {};
  int      mRowCount = 0;
  int      mOverflow = 0;
  bool     mFed = false;
  uint64_t mPrimsSeen = 0, mGp0Anon = 0, mSpanMiss = 0, mUnscopedNative = 0;
};
