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
#include <stdlib.h>         // strtoul — loadClaims parses the persisted DB
#include <string.h>          // strcmp — the native-only iid collision check (see note())
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
    // The name the FIRST producer to claim this row declared, by pointer (a string literal, same
    // contract as ProducerScope's `name`). It exists for two reasons and one of them is load-bearing:
    // a native-only row is keyed by an interned hash, so without the name the row is unidentifiable in
    // the DB; and holding the first name is what lets an iid COLLISION be detected instead of silently
    // merging two PC-only producers into one row.
    const char* name = "";
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
  // `name` is the open scope's declared name (ProducerScopeState::currentName()) and is OPTIONAL only
  // so that every existing call site keeps compiling; pass it wherever it is available, because it is
  // the only thing that makes a native-only (PC-only) row identifiable and iid collisions detectable.
  void noteNative(ProducerKey key, uint32_t prims, uint32_t frame, const char* name = nullptr) {
    noteNativeLayer(key, prims, frame, /*layer=*/-1, name);
  }
  // Same, but records WHICH DRAW LAYER an UNSCOPED push came from. Without this the report can only say
  // "1.3M prims were drawn by nobody", which names the size of the problem and not its location — and
  // the obvious way to find the location (guess a producer, scope it, re-run) is exactly the
  // re-derivation this DB exists to replace. The layer is free at the call site, so the report can rank
  // the un-declared work by pass and say which producer family to scope next.
  void noteNativeLayer(ProducerKey key, uint32_t prims, uint32_t frame, int layer,
                       const char* name = nullptr) {
    if (!key.valid()) {
      mFed = true;
      mUnscopedNative += prims;
      mPrimsSeen += prims;
      if (layer >= 0 && layer < LAYER_CAP) mUnscopedByLayer[layer] += prims;
      else                                 mUnscopedLayerUnknown += prims;
      return;
    }
    note(key, prims, frame, /*native=*/true, name);
  }
  // Prims the census SAW and could not attribute. Recording these is what makes a coverage number
  // honest; dropping them is what makes "100% attributed" meaningless.
  // Of the prims whose span had NO emitter fn, how many still carried a render-walk NODE? That is the
  // denominator for the candidate fix of keying the guest leg on the node's render fn (node+0x18), which
  // would land in the SAME key space the native leg uses. Counted rather than reasoned about, because
  // "the node is probably there" is exactly the assumption that costs a session.
  void noteSpanNoFnHadNode(uint32_t prims) { mSpanNoFnWithNode += prims; }
  uint64_t spanNoFnWithNode() const { return mSpanNoFnWithNode; }
  // Guest-leg prims whose producer was named by the NODE's render fn rather than by the span's emitter
  // fn. Counted separately and reported, because two identity SOURCES feeding one column is exactly the
  // kind of quiet conflation that makes a row untrustworthy later: a reader must be able to see how a
  // row was named, not just what it was named.
  void noteGuestViaNode(uint32_t prims) { mGuestViaNode += prims; }
  uint64_t guestViaNode() const { return mGuestViaNode; }
  // Named by the store's guest PC. Its own channel because it is the WEAKEST of the three routes —
  // c->pc is not restored when a nested call returns, so it can name a callee rather than the submitter.
  void noteGuestViaPc(uint32_t prims) { mGuestViaPc += prims; }
  uint64_t guestViaPc() const { return mGuestViaPc; }

  // GUEST-ORIGIN prims arriving at the NATIVE queue's chokepoint, counted apart from undeclared native
  // work — and this distinction is not cosmetic, it decides whether a number can ever reach zero.
  //
  // `unscopedNative()` means "real drawing by a native producer nobody has DECLARED yet", and its whole
  // value is that it names remaining work. But the guest's own GP0 linked-list walk also pushes into this
  // queue on any leg that walks the guest OT, and a guest prim has NO native producer by definition — it
  // can never be declared. Counting it as undeclared-native asserted something false and made the headline
  // number unreachable: measured 2026-08-12, native attribution read exactly 100% at 4:3 and 99.92% at
  // 16:9, and the whole 418-prim difference was this, not a widescreen producer gap.
  //
  // The trap it sets is worse than the miscount. The obvious way to drive "undeclared" to zero is to open
  // a ProducerScope on the guest function that pushed — which mints exactly the false row the producer DB
  // exists to prevent. So this is a SEPARATE, REPORTED number rather than a silent exclusion: it says
  // "these prims came from the guest, they are not yours to declare", which is a different sentence from
  // "these are attributed".
  void noteGuestOriginPush(uint32_t prims) { mFed = true; mPrimsSeen += prims; mGuestOrigin += prims; }
  uint64_t guestOrigin() const { return mGuestOrigin; }

  void noteUnattributable(Why why, uint32_t prims) {
    mFed = true;
    mPrimsSeen += prims;
    if (why == WHY_GP0_ANON)        mGp0Anon += prims;
    else if (why == WHY_SPAN_NO_FN) mSpanNoFn += prims;
    else                            mSpanMiss += prims;
  }

  // ---- read -------------------------------------------------------------------------------------
  // ---- THE CLAIM SET — which guest addresses a NATIVE producer has keyed a row at ----------------
  //
  // This is what lets the two legs meet in ONE row. The guest leg's raw identity is the innermost
  // EMITTER frame, while a native row is keyed at the HANDLER/PASS frame, so keying the guest leg on its
  // emitter puts the legs in DISJOINT rows and the DB looks complete while comparing nothing (measured:
  // 2 of 25 keys coincided). Resolution walks the call chain outward from the emitter and takes the
  // first frame THIS SET contains — i.e. the frame a native producer already claims.
  //
  // MEMBERSHIP IS EARNED BY A NATIVE PRODUCER, not by being override-installed. Every native-owned
  // function is installed, most of them nothing to do with drawing, so resolving against the override
  // table would happily key a prim at a scheduler leaf — a confident wrong row, which is worse than an
  // honest unclaimed one.
  //
  // KNOWN LIMIT, stated because it decides which leg can be compared: the set is populated as native
  // producers RUN, so on a pure psx_render leg (no native producer runs at all) it stays EMPTY and every
  // guest row is unclaimed. That is not a bug to paper over — with no native leg there is nothing to
  // compare against. The leg where the comparison is real is `PSXPORT_GATE=1` with pc_render, where the
  // substrate still fills the packet pool underneath while native producers draw. Making the set survive
  // across runs (loading it from the persisted DB) is the follow-up that would let the pure leg join too.
  // LOADING THE CLAIM SET FROM THE PERSISTED DB — what makes the two legs joinable AT ALL.
  //
  // Measured 2026-08-12, and it is a structural fact rather than a wiring gap: NO SINGLE LEG runs both
  // halves. On pc_render the guest packets are never GP0-executed, so the guest column is structurally 0;
  // on psx_render no native producer runs, so the claim set is empty and nothing resolves. A claim set
  // built only from THIS run therefore cannot ever join them. Reading it back from the DB the previous
  // run wrote does: a pc_render run records which addresses natives key, and the psx_render run that
  // follows resolves its guest prims into those same rows. That is also the user's stated design — the DB
  // accumulates across play sessions rather than describing one run.
  //
  // Deliberately a DUMB SCAN for `"key":"0x…"` on lines whose `prims_native` is non-zero: the file is the
  // framework's own output, a few hundred lines, read once at startup. A JSON parser here would be more
  // code with more ways to be wrong about a format we control on both ends.
  //
  // REFUSES LOUDLY RATHER THAN RETURNING EMPTY. A missing or unreadable DB is reported, because an empty
  // claim set silently turns every guest prim into "has no native producer" — a false negative that reads
  // exactly like a real finding. The count loaded is always printed, so "0 claims" can never pass for
  // "nothing has a native producer".
  // THE BUILD IDENTITY OF THE RUNNING CODE, injected by the caller (producer_db.cpp reads it from the
  // generated psxport_build_id.h) rather than #included here — this header must stay hermetic, and a test
  // has to be able to inject any identity, INCLUDING the `UNKNOWN(<reason>)` shape the generator emits
  // when it could not describe a repo. Nothing here manufactures an identity: an unset id stays empty and
  // the provenance report REFUSES to classify rather than comparing against "".
  static constexpr int BUILD_ID_CAP = 96;
  void setBuildId(const char* id) {
    mBuildId[0] = 0;
    if (!id) return;
    size_t n = 0;
    // The id is written into a WHITESPACE-DELIMITED claim-file column, so a space in it would split into
    // two fields and silently corrupt every later parse. Sanitised at the boundary, visibly (`_`).
    for (const char* p = id; *p && n + 1 < BUILD_ID_CAP; ++p) {
      const unsigned char ch = (unsigned char)*p;
      mBuildId[n++] = (ch > 0x20 && ch < 0x7F) ? (char)ch : '_';
    }
    mBuildId[n] = 0;
  }
  const char* buildId() const { return mBuildId; }
  // An id of the form `UNKNOWN(<reason>)` is the generator saying NO IDENTITY WAS AVAILABLE. It must never
  // take part in an equality test: two builds that both failed to describe themselves are not the same
  // build, and treating them as equal is exactly the false negative the DB exists to stop reporting.
  static bool buildIdIsReal(const char* id) {
    return id && id[0] && strncmp(id, "UNKNOWN(", 8) != 0;
  }

  int loadClaims(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
      lucent::warn("producers", "claim set NOT loaded: cannot open {} — every guest prim will resolve to "
                                "'no native producer' for that reason alone, which is NOT a measurement. "
                                "Run a pc_render leg first to write a DB, or point PSXPORT_PRODUCERS_DB at one.",
                   path);
      return 0;
    }
    char line[2048];
    int loaded = 0, skippedNoNative = 0, dup = 0, provUpdated = 0, provAbsent = 0;
    while (fgets(line, sizeof line, f)) {
      // Two accepted shapes, because the flat claim file and the run JSONL are both legitimate inputs and
      // guessing which one a path is would be a silent failure. A bare `0x…` line is a claim by
      // construction (nothing else is written to that file); a JSONL row must prove prims_native > 0.
      if (line[0] == '0' && (line[1] == 'x' || line[1] == 'X')) {
        const uint32_t a = (uint32_t)strtoul(line, nullptr, 16);
        if (!a) continue;
        // PROVENANCE COLUMNS, and the OLD ONE-COLUMN FILE STILL LOADS: `strtoul` already ignored the tail,
        // so a bare `0x…` line simply yields no provenance — which is a REAL third state ("written by a
        // pre-provenance build"), not a defect to paper over with a placeholder id.
        const char* prov = claimLineBuildId(line);
        if (!prov) provAbsent++;
        // A LATER LINE FOR AN ADDRESS ALREADY IN THE SET *UPDATES* ITS PROVENANCE, and this is the whole
        // reason the report can say "last earned by". The file is append-only and therefore chronological,
        // so the newest line for an address is the newest EARN event; keeping the first one (the previous
        // behaviour, a plain `dup++; continue;`) would have made every claim report the OLDEST build that
        // ever earned it and read as a fossil forever after one stale block.
        const int at = claimIndexOf(a);
        if (at >= 0) {
          dup++;
          if (prov) { setClaimProv(at, prov); provUpdated++; }
          continue;
        }
        if (mClaimCount < CLAIM_CAP) {
          const int i = mClaimCount++;
          mClaims[i] = a;
          mClaimFromDisk[i] = true;
          setClaimProv(i, prov);
          loaded++;
        } else mClaimOverflow++;
        continue;
      }
      const char* pn = strstr(line, "\"prims_native\":");
      if (!pn) continue;
      if (strtoul(pn + 16, nullptr, 10) == 0) { skippedNoNative++; continue; }   // no native producer -> not a claim
      const char* k = strstr(line, "\"key\":\"0x");
      if (!k) continue;
      const uint32_t addr = (uint32_t)strtoul(k + 9, nullptr, 16);
      if (!addr) continue;
      const int at = claimIndexOf(addr);
      if (at >= 0) { dup++; continue; }
      if (mClaimCount < CLAIM_CAP) {
        const int i = mClaimCount++;
        mClaims[i] = addr;
        mClaimFromDisk[i] = true;
        setClaimProv(i, nullptr);      // a JSONL row carries no claim-file provenance column
        provAbsent++;
        loaded++;
      }
      else                         { mClaimOverflow++; }
    }
    fclose(f);
    lucent::info("producers", "claim set loaded from {}: {} address(es) ({} row(s) skipped as having no "
                              "native producer, {} duplicate line(s) of which {} carried NEWER provenance "
                              "that superseded an earlier line, {} line(s) carried NO build id at all, {} "
                              "lost to CLAIM_CAP={}). These are the rows a guest prim can be JOINED to.",
                 path, loaded, skippedNoNative, dup, provUpdated, provAbsent, mClaimOverflow, CLAIM_CAP);
    return loaded;
  }

  // The claim set is persisted APPEND-ONLY and separately from the per-run JSONL, on purpose. The obvious
  // alternative — reload the newest run file — silently DESTROYS the set: a psx_render run's rows all have
  // prims_native 0, so the newest file legitimately contains no claims, and loading it would report "no
  // effect has a native producer" about a game where nine of them do. Accumulating instead means a claim,
  // once earned by a native producer actually drawing, stays a claim until someone deletes the file.
  // WHAT THIS WRITES IS WHAT THIS RUN *EARNED*, and that is a bug fix, not a policy change (kanban #91).
  // It used to write `mClaims[0..mClaimCount)` — the UNION of the loaded set and the earned set, because
  // `loadClaims` fills the same array `note()` does. So every run re-emitted the whole file's contents and
  // the NEWEST block always looked freshly earned; the on-disk file could not answer "what did this run
  // earn" even in principle, which is how a claim whose producer KEY had MOVED kept reading as live.
  // Re-emitting a loaded claim is also what would DESTROY its provenance: the only honest build id for a
  // claim this run did not earn is the one already on its old line, and leaving that line alone preserves
  // it. Append-only is UNCHANGED and still right for the reason its original note gives — absence on one
  // leg must never un-earn a claim, because no single leg runs both halves.
  int appendClaims(const char* path, const char* runStamp) const {
    int earned = 0;
    for (int i = 0; i < mClaimCount; i++) if (mClaimEarnedHere[i]) earned++;
    if (earned == 0) {
      // NOT SILENT. "This run earned nothing new" is a real and common outcome (a pure psx_render leg, or a
      // native leg that revisited only content it had already claimed) and it must be distinguishable from
      // a failed write, which is what an early `return 0` with no line looked like before.
      lucent::info("producers", "claim set: appended 0 address(es) -> {} — this run EARNED no claim "
                                "({} claim(s) were loaded from disk and are deliberately NOT re-emitted; "
                                "re-emitting them is the union-echo bug kanban #91 is about)",
                   path, mClaimCount);
      return 0;
    }
    FILE* f = fopen(path, "a");
    if (!f) {
      lucent::warn("producers", "claim set NOT persisted: cannot append to {} — the next run will resolve "
                                "fewer guest prims and will say so", path);
      return 0;
    }
    // Line format `0xADDRESS <run-stamp> <build-id>`. Deliberately whitespace-columnar and address-first:
    // `loadClaims`'s existing `line[0]=='0'` branch strtoul's the address and IGNORES the tail, so a file
    // written here still loads in a PRE-provenance build, and a pre-provenance file still loads here.
    for (int i = 0; i < mClaimCount; i++) {
      if (!mClaimEarnedHere[i]) continue;
      fprintf(f, "0x%08X %s %s\n", mClaims[i],
              (runStamp && runStamp[0]) ? runStamp : "no-stamp",
              mBuildId[0] ? mBuildId : "UNKNOWN(no-build-id-set)");
    }
    fclose(f);
    lucent::info("producers", "claim set: appended {} address(es) EARNED THIS RUN -> {} (of {} in the set; "
                              "the other {} were loaded from disk and keep the provenance of the run that "
                              "earned them). Stamped build id: {}. Append-only: a claim earned by a native "
                              "producer drawing is never un-earned by a later leg that skips it.",
                 earned, path, mClaimCount, mClaimCount - earned,
                 mBuildId[0] ? mBuildId : "NONE — the caller never called setBuildId()");
    return earned;
  }

  bool claims(uint32_t addr) const {
    for (int i = 0; i < mClaimCount; i++) if (mClaims[i] == addr) return true;
    return false;
  }
  int claimCount() const { return mClaimCount; }
  uint32_t claimAt(int i) const { return (i >= 0 && i < mClaimCount) ? mClaims[i] : 0; }
  // ---- THE UNION SPLIT (kanban #91 step 1) --------------------------------------------------------
  // `claimCount()` is a UNION and always was: `loadClaims` and `note()` fill the same array, on purpose —
  // the resolver wants every address a guest prim may join to, whoever earned it. What was missing is that
  // NOTHING could tell the two apart afterwards, so a claim earned five commits ago and one earned in this
  // very run were indistinguishable. These two predicates are that distinction, and everything provenance
  // downstream (the file writer, the report, the tests) is built on them rather than on array position.
  bool claimEarnedHere(int i) const { return (i >= 0 && i < mClaimCount) && mClaimEarnedHere[i]; }
  bool claimFromDisk(int i)   const { return (i >= 0 && i < mClaimCount) && mClaimFromDisk[i]; }
  // The build id the claim FILE recorded for this claim; "" when the line carried none (a pre-provenance
  // build wrote it). Never synthesised — see the note on `UNKNOWN(...)` above.
  const char* claimProv(int i) const { return (i >= 0 && i < mClaimCount) ? mClaimProv[i] : ""; }
  int claimEarnedHereCount() const {
    int n = 0;
    for (int i = 0; i < mClaimCount; i++) if (mClaimEarnedHere[i]) n++;
    return n;
  }
  int claimLoadedCount() const {
    int n = 0;
    for (int i = 0; i < mClaimCount; i++) if (mClaimFromDisk[i]) n++;
    return n;
  }
  // Stores attributed while the set was still EMPTY — the ordering blind spot, COUNTED rather than
  // argued about. A prim drawn before the frame's first native producer ran cannot be resolved, so it
  // lands on its emitter key. If this is large, early rows are emitter-keyed and the join is partial.
  uint64_t claimResolveTooEarly() const { return mClaimTooEarly; }
  void noteClaimTooEarly() { mClaimTooEarly++; }

  int rowCount() const { return mRowCount; }
  const Row* rowAt(int i) const { return (i >= 0 && i < mRowCount) ? &mRows[i] : nullptr; }
  const Row* find(ProducerKey key) const {
    for (int i = 0; i < mRowCount; i++) if (mRows[i].key == key) return &mRows[i];
    return nullptr;
  }

  bool     wasFed()          const { return mFed; }
  uint64_t primsSeen()       const { return mPrimsSeen; }
  uint64_t primsAttributed() const {
    return mPrimsSeen - mGp0Anon - mSpanMiss - mSpanNoFn - mUnscopedNative - mGuestOrigin;
  }
  uint64_t gp0Anon()         const { return mGp0Anon; }
  uint64_t spanMiss()         const { return mSpanMiss; }
  uint64_t spanNoFn()        const { return mSpanNoFn; }
  uint64_t unscopedNative()  const { return mUnscopedNative; }
  int      overflow()        const { return mOverflow; }
  // PC-only producers whose interned iid landed on a row already claimed under a DIFFERENT name. Zero is
  // the normal answer; non-zero means two producers are sharing one row and BOTH rows' numbers are wrong.
  int      iidCollisions()   const { return mIidCollisions; }
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
  // Ownership query injected by the caller (overrides::query) rather than #included here, so this header
  // stays hermetic — tests/test_producer_scope.cpp exercises the census with no registry linked at all.
  // Returns false when the address is not override-installed; fills the native hit count on true.
  typedef bool (*OwnedQueryFn)(uint32_t addr, uint64_t* nativeHits, uint64_t* oracleHits);

  void writeJsonl(const char* path, const char* stamp, OwnedQueryFn ownedQuery = nullptr) const {
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
      // OWNERSHIP, and the three states are deliberately distinct. Without a query we must not write
      // `false`: "the address is not override-installed" and "nobody asked" are different facts, and
      // collapsing them is how the DB's own report line came to print one answer forever — the fields
      // were consumed as "derived by the runtime" while nothing ever emitted them.
      char owned[96];
      if (r.key.isNativeOnly()) {
        // A PC-ONLY ROW HAS NO GUEST FUNCTION TO OWN, so the ownership question does not apply and must
        // not be answered. Two things would go wrong otherwise: the query would be handed an INTERNED ID
        // as if it were a guest address (a hash can coincide with an installed address), and
        // `has_native:false` — the shape a reader takes as "no native producer exists" — would be
        // stamped on the one row that is native BY CONSTRUCTION. This is the fourth distinct state,
        // alongside true / false / "nobody asked".
        snprintf(owned, sizeof owned, ",\"owned_query\":\"pc-only\"");
      } else if (!ownedQuery) {
        snprintf(owned, sizeof owned, ",\"owned_query\":\"unavailable\"");
      } else {
        uint64_t nativeHits = 0, oracleHits = 0;
        const bool registered = ownedQuery(r.key.id, &nativeHits, &oracleHits);
        // BOTH hit counts, because native_reached==false has TWO causes with different meanings and
        // they are indistinguishable from the native count alone: the address was never dispatched at
        // all (the guest path does not execute on this leg), or it WAS dispatched and routed to the
        // substrate gen body (oracle leg / forced). The first says the override is unexercised; the
        // second says it ran as the reference. Reporting only nativeHits collapses them into one row
        // that reads like the first — the same conflation that made these fields useless to begin with.
        snprintf(owned, sizeof owned,
                 ",\"has_native\":%s,\"native_reached\":%s,\"native_hits\":%llu,\"oracle_hits\":%llu",
                 registered ? "true" : "false",
                 (registered && nativeHits > 0) ? "true" : "false",
                 (unsigned long long)nativeHits, (unsigned long long)oracleHits);
      }
      // `pc_name` is the OBSERVED scope name, kept separate from producers.py's CURATED `name:` — the
      // runtime must not overwrite a human's label, and for a native-only row it is the only thing that
      // says which code the interned key belongs to.
      fprintf(f,
              "{\"key\":\"%s\",\"kind\":\"%s\",\"pc_name\":\"%s\","
              "\"prims_guest\":%u,\"prims_native\":%u,"
              "\"frames\":%u,\"first_frame\":%u,\"last_frame\":%u%s,\"seen_at\":\"%s\"}\n",
              keyStr(r.key).s, r.key.isNativeOnly() ? "native-only" : "guest", jsonSafe(r.name).s,
              r.primsGuest, r.primsNative, r.frames, r.firstFrame, r.lastFrame, owned, stamp ? stamp : "");
    }
    // The denominators travel WITH the rows. Without them a later reader can compute a coverage
    // percentage from the rows alone and be confidently wrong, because the rows say nothing about the
    // prims that could not be attributed at all.
    fprintf(f,
            "{\"type\":\"totals\",\"prims_seen\":%llu,\"prims_attributed\":%llu,"
            "\"gp0_anon\":%llu,\"span_miss\":%llu,\"span_no_fn\":%llu,\"guest_origin\":%llu,"
            "\"unscoped_native\":%llu,\"overflow\":%d,\"iid_collisions\":%d,"
            "\"rows\":%d,\"claims_earned_here\":%d,\"claims_loaded\":%d,"
            "\"build_id\":\"%s\",\"seen_at\":\"%s\"}\n",
            (unsigned long long)mPrimsSeen, (unsigned long long)primsAttributed(),
            (unsigned long long)mGp0Anon, (unsigned long long)mSpanMiss,
            (unsigned long long)mSpanNoFn, (unsigned long long)mGuestOrigin,
            (unsigned long long)mUnscopedNative, mOverflow, mIidCollisions, mRowCount,
            claimEarnedHereCount(), claimLoadedCount(),
            // THE RUN'S OWN BUILD IDENTITY, travelling with the rows. `tools/producers.py stale` states
            // as its residual that "nothing here can say which PSXPORT_DIR a build came from" — it
            // identifies a leg's binary by an md5 the harness records, which `touch` cannot fake but which
            // also cannot name the CODE. This field is that name. It is written EXACTLY as held: when no
            // identity was set it is the empty string, never a placeholder, because a reader must be able
            // to tell "this run had no identity" from "this run was build ''".
            mBuildId, stamp ? stamp : "");
    fclose(f);
    lucent::info("producers", "wrote {} row(s) + totals -> {}", mRowCount, path);
  }

  // ---- CLAIM-SET PROVENANCE, printed UNCONDITIONALLY (kanban #91 step 3) --------------------------
  //
  // THE QUESTION: of the claims this run loaded from disk, which were last earned by a DIFFERENT build than
  // the one running? Those are the ones whose attribution of guest prims may be a FOSSIL of a producer key
  // that moved — the concrete case is Tomba!2's `perObjFlush`, whose key is the per-mode emitter and which
  // therefore moved from 0x800803DC to 0x80146478 when the routing changed, leaving a claim on disk that no
  // build since can earn in mode-0 content.
  //
  // THE NEGATIVE IS DESIGNED FIRST, three ways, because every one of them was a way to lie silently:
  //
  //  1. NOT RE-EARNED IS NOT DEAD, and the line says so in those words. A producer key is a FUNCTION OF
  //     GUEST STATE: 12 of Tomba!2's 22 MODE_TABLE entries select 0x800803DC, so a claim goes un-earned
  //     whenever the run never visited the content that earns it. Measured, on that game: 119 historical
  //     native legs missed 0x800803DC purely because the whole corpus was mode-0 seaside content. A report
  //     that said "stale" would have had a session prune a claim that is live in over half the game.
  //  2. NO BUILD IDENTITY -> REFUSE TO CLASSIFY. With no id set, every comparison would be against "" and
  //     every claim would read DIFFERENT-BUILD. The report says the comparison COULD NOT BE MADE instead.
  //  3. `UNKNOWN(<reason>)` IS NOT AN ID. Two builds that both failed to describe themselves are not the
  //     same build, so an UNKNOWN on either side is its own class and never an equal or an unequal.
  //
  // It prints even when the set is empty and even when nothing is wrong, because "the set holds nothing
  // from a foreign build" is a finding only if the reader can see the denominator it was drawn from.
  void reportClaimProvenance(const char* who) const {
    const int loaded = claimLoadedCount();
    const int earnedHere = claimEarnedHereCount();
    const int fresh = mClaimCount - loaded;   // earned this run and never seen on disk before
    if (!buildIdIsReal(mBuildId)) {
      lucent::warn("producers",
                   "{}: claim set provenance CANNOT BE COMPUTED — this run has no usable build identity "
                   "({}). {} claim(s) in the set, {} loaded from disk, {} re-earned by this run. Whether "
                   "any of the loaded ones were last earned by a DIFFERENT build is UNKNOWN, not 'none': "
                   "the comparison was never made. Fix the build-id generator (cmake/build_id.cmake) "
                   "rather than reading the numbers above as an all-clear.",
                   who, mBuildId[0] ? mBuildId : "setBuildId() was never called",
                   mClaimCount, loaded, earnedHere);
      return;
    }
    int reEarned = 0, otherBuild = 0, sameBuildCold = 0, noProv = 0, provUnknown = 0;
    const char* firstOther = nullptr;
    uint32_t firstOtherAddr = 0;
    for (int i = 0; i < mClaimCount; i++) {
      if (!mClaimFromDisk[i]) continue;
      if (mClaimEarnedHere[i]) { reEarned++; continue; }
      const char* p = mClaimProv[i];
      if (!p[0])                    { noProv++; continue; }
      if (!buildIdIsReal(p))        { provUnknown++; continue; }
      if (strcmp(p, mBuildId) == 0) { sameBuildCold++; continue; }
      otherBuild++;
      if (!firstOther) { firstOther = p; firstOtherAddr = mClaims[i]; }
    }
    char firstOtherNote[160] = "";
    if (firstOther)
      snprintf(firstOtherNote, sizeof firstOtherNote, " (first: 0x%08X, last earned by %s)",
               firstOtherAddr, firstOther);
    lucent::info("producers",
                 "{}: claim set provenance — build id {}. {} claim(s) in the set = {} loaded from disk + {} "
                 "first earned in this run. Of the {} loaded: {} RE-EARNED by this build in this run, {} "
                 "last earned by a DIFFERENT build{}, {} last earned by THIS build id but not re-earned in "
                 "this run, {} carry NO provenance (written by a pre-provenance build), {} carry an "
                 "UNKNOWN(...) id which is NOT comparable and so is neither.",
                 who, mBuildId, mClaimCount, loaded, fresh, loaded, reEarned, otherBuild, firstOtherNote,
                 sameBuildCold, noProv, provUnknown);
    // THE BLIND SPOT, and it is louder than the numbers on purpose — this is the sentence whose absence
    // would make a reader draw the exactly wrong conclusion from the line above.
    const int notReEarned = otherBuild + sameBuildCold + noProv + provUnknown;
    if (notReEarned) {
      lucent::warn("producers",
                   "{}: {} loaded claim(s) were NOT re-earned by this run — of which {} were last earned by "
                   "a DIFFERENT build, so their attribution of guest prims is POSSIBLY A FOSSIL of a "
                   "producer key that MOVED. NOT RE-EARNED IS NOT DEAD: a producer's key is a function of "
                   "GUEST STATE (Tomba!2's perObjFlush keys on the per-mode emitter, and 12 of 22 render "
                   "modes select one single address), so a claim goes un-earned whenever this run never "
                   "visited the content that earns it. REACH THE CONTENT before treating any of these as "
                   "dead, and do not prune them.",
                   who, notReEarned, otherBuild);
    }
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
                 "{}: {} row(s); prims seen {} = attributed {} + unscoped-native {} + guest-origin {} + "
                 "gp0-anon {} + span-miss {} + span-no-fn {}{}",
                 who, mRowCount, (unsigned long long)mPrimsSeen,
                 (unsigned long long)primsAttributed(), (unsigned long long)mUnscopedNative,
                 (unsigned long long)mGuestOrigin,
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
        lucent::info("producers", "  {} {}  native {}  guest {}  frames {} (f{}..f{})  {}",
                     r.key.isNativeOnly() ? "pc-only" : "guest  ", keyStr(r.key).s,
                     r.primsNative, r.primsGuest, r.frames, r.firstFrame, r.lastFrame, r.name);
      }
      if (n > shown)
        lucent::info("producers", "  … {} more row(s) not shown (full set goes to the JSONL writer)",
                     n - shown);
    }
    if (mIidCollisions) {
      lucent::error("producers",
                    "{}: {} PC-only push(es) hit an iid ALREADY CLAIMED BY A DIFFERENT PRODUCER — first "
                    "clash: pc-{:08X} claimed by '{}', then pushed by '{}'. Both rows' prim counts are "
                    "now wrong; rename one producer's stable id.",
                    who, mIidCollisions, mCollideIid,
                    mCollideA ? mCollideA : "?", mCollideB ? mCollideB : "?");
    }
    if (mGuestViaNode) {
      lucent::info("producers",
                   "{}: {} guest prim(s) were named by the NODE's render fn (node+0x18) rather than by "
                   "the span's emitter fn — the same key space the native leg uses, so both legs land in "
                   "one row.", who, (unsigned long long)mGuestViaNode);
    }
    if (mGuestViaPc) {
      lucent::warn("producers",
                   "{}: {} guest prim(s) were named only by the store's guest PC — the WEAKEST route "
                   "(c->pc is not restored after a nested call, so it may name a callee rather than the "
                   "submitter). Treat those rows as provisional until a stronger identity replaces them.",
                   who, (unsigned long long)mGuestViaPc);
    }
    if (mSpanNoFn) {
      lucent::warn("producers",
                   "{}: {} prim(s) had a span with NO emitter fn — of those, {} DID carry a render-walk "
                   "node. That second number is the denominator for keying the guest leg on the node's "
                   "render fn (node+0x18); if it is 0 that candidate is dead, not merely unexplored.",
                   who, (unsigned long long)mSpanNoFn, (unsigned long long)mSpanNoFnWithNode);
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
  static constexpr int CLAIM_CAP = 256;
  static constexpr int PROV_CAP  = BUILD_ID_CAP;
  uint32_t mClaims[CLAIM_CAP] = {};
  // Parallel to mClaims, and parallel rather than a struct only because mClaims' layout is what
  // `reportChains` and `claimAt` already hand out; a struct here would churn three call sites for nothing.
  bool     mClaimEarnedHere[CLAIM_CAP] = {};   // a native producer pushed on this GUEST key IN THIS RUN
  bool     mClaimFromDisk[CLAIM_CAP]   = {};   // the claim existed before this run started
  char     mClaimProv[CLAIM_CAP][PROV_CAP] = {};   // build id the FILE recorded; "" = none recorded
  char     mBuildId[BUILD_ID_CAP] = {};        // identity of the RUNNING code; "" = caller never set one
  int      mClaimCount = 0;
  int      mClaimOverflow = 0;     // claims lost to a full set — an UNCLAIMED prim may be this, not a miss
  uint64_t mClaimTooEarly = 0;

  int claimIndexOf(uint32_t addr) const {
    for (int i = 0; i < mClaimCount; i++) if (mClaims[i] == addr) return i;
    return -1;
  }
  void setClaimProv(int i, const char* prov) {
    mClaimProv[i][0] = 0;
    if (!prov) return;
    size_t n = 0;
    while (prov[n] && (unsigned char)prov[n] > 0x20 && n + 1 < PROV_CAP) { mClaimProv[i][n] = prov[n]; n++; }
    mClaimProv[i][n] = 0;
  }
  // Third whitespace-delimited field of a claim line, or nullptr when the line has no such field. Returns
  // nullptr — never "" and never a placeholder — because "this line records no build id" is a REAL state
  // the report has to name separately from "recorded a different build".
  static const char* claimLineBuildId(const char* line) {
    int field = 0;
    const char* p = line;
    while (*p) {
      while (*p == ' ' || *p == '\t') p++;
      if (!*p || *p == '\n' || *p == '\r') return nullptr;
      if (field == 2) return p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
      field++;
    }
    return nullptr;
  }

  void note(ProducerKey key, uint32_t prims, uint32_t frame, bool native, const char* name = nullptr) {
    mFed = true;
    mPrimsSeen += prims;
    // A NATIVE push on a GUEST key is what makes that address a claim — see claims() above. Registered
    // here, in the one funnel both native entry points share, so no producer can be a claim in one path
    // and not the other. Linear + deduped: the set is tens of entries, and it is read on a store path, so
    // it stays a flat array rather than becoming a hash nobody can dump.
    if (native && key.kind == ProducerKey::GUEST) {
      const int at = claimIndexOf(key.id);
      if (at >= 0) {
        // ALREADY IN THE SET — but this run has now EARNED it, and that is the fact the old code threw
        // away. A claim loaded from disk and re-earned here is not a fossil; one loaded and never re-earned
        // may be. Nothing could tell them apart, so `producer_db_finish` had nothing to report.
        mClaimEarnedHere[at] = true;
      } else if (mClaimCount < CLAIM_CAP) {
        const int i = mClaimCount++;
        mClaims[i] = key.id;
        mClaimEarnedHere[i] = true;
        mClaimFromDisk[i] = false;
        mClaimProv[i][0] = 0;         // brand new: its provenance is THIS build, written by appendClaims
      } else {
        mClaimOverflow++;
      }
    }
    Row* r = nullptr;
    for (int i = 0; i < mRowCount; i++) if (mRows[i].key == key) { r = &mRows[i]; break; }
    if (r && name && name[0]) {
      if (!r->name[0]) {
        r->name = name;
      } else if (key.isNativeOnly() && strcmp(r->name, name) != 0) {
        // TWO DIFFERENT PC-ONLY PRODUCERS ON ONE iid. The hash is not injective, so this is possible;
        // what is not acceptable is merging them quietly, which would report one row's prims as the
        // other's. Counted, and the pair is kept for the report so the fix is "rename one of these two"
        // rather than an investigation. Only for native-only keys: a guest row legitimately receives
        // pushes from more than one native under more than one name (0x8007FCC8 has two claimants —
        // `codemap.py --addr 0x8007FCC8`), so a name mismatch there is not evidence of a key collision.
        mIidCollisions++;
        if (!mCollideA) { mCollideA = r->name; mCollideB = name; mCollideIid = key.id; }
      }
    }
    if (!r) {
      if (mRowCount >= CAP) { mOverflow++; return; }   // the prim still counted as seen+attributed
      r = &mRows[mRowCount++];
      r->key = key;
      r->name = (name && name[0]) ? name : "";
      r->firstFrame = frame;
      r->lastFrame  = frame;
      r->frames     = 1;
    } else if (frame != r->lastFrame) {
      r->lastFrame = frame;
      r->frames++;
    }
    if (native) r->primsNative += prims; else r->primsGuest += prims;
  }

  // ---- the row key AS A STRING, and why native-only rows may not use the guest format --------------
  // The key string is the row's identity everywhere downstream: it is the JSONL `key`, the DB row's
  // FILENAME (docs/producers/<key>.md) and the argument to `producers.py show`. An interned iid printed
  // as `0x6E3A1C2B` would be indistinguishable from a guest address in every one of those places — a
  // row that looks like a function that does not exist. `pc-` says which space the number lives in.
  struct KeyStr { char s[24]; };
  static KeyStr keyStr(ProducerKey k) {
    KeyStr o{};
    snprintf(o.s, sizeof o.s, k.isNativeOnly() ? "pc-%08X" : "0x%08X", k.id);
    return o;
  }
  // Names come from source string literals, but this writes JSON that a tool INGESTS: one stray quote
  // and producers.py counts the whole record unparseable, so a run's rows vanish from the DB. Sanitised
  // rather than trusted; truncation is visible in the value itself.
  struct SafeStr { char s[64]; };
  static SafeStr jsonSafe(const char* in) {
    SafeStr o{};
    size_t n = 0;
    for (const char* p = in ? in : ""; *p && n + 1 < sizeof o.s; ++p) {
      const unsigned char c = (unsigned char)*p;
      o.s[n++] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '_';
    }
    o.s[n] = 0;
    return o;
  }

  int         mIidCollisions = 0;
  const char* mCollideA = nullptr;
  const char* mCollideB = nullptr;
  uint32_t    mCollideIid = 0;
  uint64_t mSpanNoFn = 0;
  uint64_t mSpanNoFnWithNode = 0;
  uint64_t mGuestViaNode = 0;
  uint64_t mGuestViaPc = 0;
  uint64_t mUnscopedByLayer[LAYER_CAP] = {};
  uint64_t mUnscopedLayerUnknown = 0;
  Row      mRows[CAP] = {};
  int      mRowCount = 0;
  int      mOverflow = 0;
  bool     mFed = false;
  uint64_t mPrimsSeen = 0, mGp0Anon = 0, mSpanMiss = 0, mUnscopedNative = 0;
  uint64_t mGuestOrigin = 0;   // guest-origin pushes at the native chokepoint (see noteGuestOriginPush)
};
