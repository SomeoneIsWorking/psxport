// producer_scope.h — "WHICH native producer is drawing right now", for the graphics-producer DB's
// NATIVE leg (docs/plans/graphics-producer-db.md stage 3).
//
// WHAT IT IS FOR. The DB compares, per effect, what the guest submits through the GTE/OT against what a
// native producer draws. The guest leg gets its identity for free — the packet-pool store carries the
// submitting fn (OtAttr). The native leg has no such trail: `drawWorldQuad` is a shared function called
// by everything, so a prim arriving there is anonymous unless the producer SAYS who it is. This is that
// declaration, and it is the whole native leg of the compare.
//
// THE ONE RULE THAT MAKES IT HONEST: A PUSH WITH NO SCOPE OPEN IS COUNTED, NOT DROPPED.
// `ProducerCensus::noteNative` routes an invalid key to `unscopedNative()`, so undeclared native drawing
// shows up as a real number. It must never be attributed to "whichever producer was last", and it must
// never be silently discarded: undeclared work is precisely the row the DB exists to surface, and
// dropping it would let the census report "every native prim is attributed" over a picture half of which
// came from nowhere. That reads as completeness, which is the failure mode this project keeps paying for.
//
// RESTORE, DO NOT CLEAR. Nesting is real — a controller producer calls a shared writer producer, and the
// writer's scope must not leave the controller's later prims unattributed on the way out. So the
// destructor restores the previous producer, exactly as the game-side `ObjScope` does for node identity
// (Tomba2Engine/game/render/render_internal.h, whose comment records why clearing was wrong there too).
//
// HOST-SIDE ONLY. The scope state is a couple of host words. It writes NO guest memory, so wrapping a
// byte-exact walk with it is free and cannot affect an SBS compare — the same property that makes
// diagnostics exempt from the "picture comes from game state" rule (PROTOCOL.md): this ANSWERS A
// QUESTION, it never produces the picture.
//
// IDENTITY. A native producer that reimplements a guest submitter is keyed by that GUEST ADDRESS, so its
// row is the SAME row the guest leg feeds and the two legs land side by side — which is the entire
// point of the comparison. A PC-only producer (an enhancement with no guest counterpart) uses
// `ProducerKey::native(iid)`, a deliberately separate id space; see ProducerKey's own comment and
// `pc_producer()` below, which is how a PC-only producer OPENS a scope.
#pragma once
#include "producer_census.h"
#include <stdint.h>

// ---- PC-ONLY producer identity: how an enhancement with no guest counterpart names itself ---------
//
// WHY IT CANNOT BORROW A GUEST ADDRESS. A PC-only producer (widescreen pillarbox/margins, an
// interpolated overlay) has no guest submitter, so putting it inside a guest-keyed scope FABRICATES a
// discrepancy in the one column the DB exists to compare: the widescreen pillarbox quad added exactly
// one native prim against the guest's one, making a faithful producer read 2-vs-1
// (Tomba2Engine/game/render/render_options.cpp records why it was left OUTSIDE the scope instead). The
// honest alternative before this existed was to leave it undeclared — counted as unscopedNative(),
// i.e. inside the number that is supposed to mean "a producer nobody has written down yet".
//
// HOW A PRODUCER PICKS ITS iid — IT DOES NOT PICK A NUMBER. The iid is the compile-time FNV-1a hash of
// a stable STRING id written at the producer's own call site (`pc/margin-render`). Nobody allocates
// ids, no central table has to be kept in sync with N game repos, and the id is greppable: the string
// that produced the row is in the source, so `grep` answers "which code is row pc-XXXXXXXX" without a
// registry. The string is an IDENTITY, NOT A CAPTION — renaming it mints a NEW row, exactly as moving a
// guest producer to a different address would. Curate the human-readable label in the DB row
// (producers.py's `name:`), not here.
//
// WHAT STOPS TWO PRODUCERS COLLIDING ON ONE iid. Nothing can make a 32-bit hash injective, so the
// collision is DETECTED rather than declared impossible: the census remembers the name of the first
// producer to claim a native-only row and counts + reports (ProducerCensus::iidCollisions()) any push
// arriving on that iid under a DIFFERENT name. A silent merge of two PC producers into one row is the
// failure mode this project keeps paying for; a counted, named collision is a rename. Note the check is
// deliberately native-only: a guest row legitimately receives pushes from more than one native under
// more than one name (0x8007FCC8 has two claimants today — `codemap.py --addr 0x8007FCC8`), so a name
// mismatch there is not evidence of a key collision and must not be reported as one.
constexpr uint32_t producer_iid(const char *s) {
  uint32_t h = 2166136261u; // FNV-1a 32, offset basis
  for (; *s; ++s) {
    h ^= (uint32_t)(unsigned char)*s;
    h *= 16777619u;
  }
  // 0 is reserved: ProducerKey{NATIVE_ONLY,0} is a VALID key, so a hash of 0 would mint a row keyed
  // zero — the same bogus "producer 0x00000000" row the guest leg already had to stop creating.
  return h ? h : 1u;
}

// A distinct TYPE, not a bare uint32_t, so a PC-only scope can never be opened by accident from an
// integer that happens to be lying around — and so the guest-address constructor stays unambiguous.
struct PcProducer {
  uint32_t iid = 0;
  const char *name = ""; // stored by pointer: a string literal, like ProducerScope's `name`
};
constexpr PcProducer pc_producer(const char *stableId) {
  return PcProducer{producer_iid(stableId), stableId};
}

// The current-producer stack, one per Core (lives on RenderSubstrate next to `diag` and `otAttr`).
// Depth is bounded and OVERFLOW IS COUNTED: a scope stack that silently stopped nesting would
// mis-attribute every prim below the overflow point, so the count is readable and reported.
class ProducerScopeState {
public:
  static constexpr int MAX_DEPTH = 16; // controller -> writer -> helper is 3; 16 is generous

  ProducerKey currentKey() const {
    // NONE when nothing is open: the explicit "I cannot name a producer" that routes to unscoped.
    // The KEY IS CARRIED, not rebuilt from an address: a PC-only scope's row lives in the native-only
    // id space, and reconstructing `guest(addr)` here is what made that space unreachable.
    return mDepth > 0 ? mStack[mDepth - 1].key : ProducerKey::none();
  }
  bool active() const {
    return mDepth > 0;
  }
  int depth() const {
    return mDepth;
  }
  // The GUEST address of the current scope, and 0 for a PC-only one — a PC-only scope has no guest
  // address, and answering with its iid here would let a caller treat an interned id as an address.
  uint32_t currentAddr() const {
    return (mDepth > 0 && mStack[mDepth - 1].key.kind == ProducerKey::GUEST) ? mStack[mDepth - 1].key.id : 0u;
  }
  const char *currentName() const {
    return mDepth > 0 ? mStack[mDepth - 1].name : "";
  }
  int overflow() const {
    return mOverflow;
  }

  // push/pop are for ProducerScope only — use the RAII type, so an early return cannot leak a scope.
  void push(uint32_t addr, const char *name) {
    pushKey(ProducerKey::guest(addr), name);
  }
  void pushKey(ProducerKey key, const char *name) {
    if (mDepth >= MAX_DEPTH) {
      mOverflow++;
      return;
    }
    mStack[mDepth].key = key;
    mStack[mDepth].name = name ? name : "";
    mDepth++;
  }
  void pop() {
    // A pop that matches a REFUSED push must not underflow the stack and steal the enclosing scope.
    if (mOverflow > 0 && mDepth >= MAX_DEPTH) {
      mOverflow--;
      return;
    }
    if (mDepth > 0) {
      mDepth--;
    }
  }

private:
  struct Entry {
    ProducerKey key;
    const char *name = "";
  };
  Entry mStack[MAX_DEPTH] = {};
  int mDepth = 0;
  int mOverflow = 0;
};

// Declare "this native producer is drawing" for a scope. `guestAddr` is the guest submitter fn this
// producer reimplements, so its row is shared with the guest leg. `name` must be a STRING LITERAL or
// otherwise outlive the scope — it is stored by pointer, not copied (these are per-prim-hot paths).
class ProducerScope {
public:
  ProducerScope(ProducerScopeState *st, uint32_t guestAddr, const char *name) : mSt(st) {
    if (mSt) {
      mSt->push(guestAddr, name);
    }
  }
  // PC-ONLY: `ProducerScope s(&st, pc_producer("pc/margin-render"));`. There is deliberately no separate
  // `name` argument — the stable string id IS the name, so the id and the label cannot drift apart, and
  // a collision report can print the two names that clashed.
  explicit ProducerScope(ProducerScopeState *st, PcProducer pc) : mSt(st) {
    if (mSt) {
      mSt->pushKey(ProducerKey::native(pc.iid), pc.name);
    }
  }
  ~ProducerScope() {
    if (mSt) {
      mSt->pop();
    }
  }
  ProducerScope(const ProducerScope &) = delete;
  ProducerScope &operator=(const ProducerScope &) = delete;

private:
  ProducerScopeState *mSt;
};
