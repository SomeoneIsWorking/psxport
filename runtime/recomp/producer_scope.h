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
// `ProducerKey::native(iid)`, a deliberately separate id space; see ProducerKey's own comment.
#pragma once
#include <stdint.h>
#include "producer_census.h"

// The current-producer stack, one per Core (lives on RenderSubstrate next to `diag` and `otAttr`).
// Depth is bounded and OVERFLOW IS COUNTED: a scope stack that silently stopped nesting would
// mis-attribute every prim below the overflow point, so the count is readable and reported.
class ProducerScopeState {
 public:
  static constexpr int MAX_DEPTH = 16;   // controller -> writer -> helper is 3; 16 is generous

  ProducerKey currentKey() const {
    // NONE when nothing is open: the explicit "I cannot name a producer" that routes to unscoped.
    return mDepth > 0 ? ProducerKey::guest(mStack[mDepth - 1].addr) : ProducerKey::none();
  }
  bool        active()      const { return mDepth > 0; }
  int         depth()       const { return mDepth; }
  uint32_t    currentAddr() const { return mDepth > 0 ? mStack[mDepth - 1].addr : 0u; }
  const char* currentName() const { return mDepth > 0 ? mStack[mDepth - 1].name : ""; }
  int         overflow()    const { return mOverflow; }

  // push/pop are for ProducerScope only — use the RAII type, so an early return cannot leak a scope.
  void push(uint32_t addr, const char* name) {
    if (mDepth >= MAX_DEPTH) { mOverflow++; return; }
    mStack[mDepth].addr = addr;
    mStack[mDepth].name = name ? name : "";
    mDepth++;
  }
  void pop() {
    // A pop that matches a REFUSED push must not underflow the stack and steal the enclosing scope.
    if (mOverflow > 0 && mDepth >= MAX_DEPTH) { mOverflow--; return; }
    if (mDepth > 0) mDepth--;
  }

 private:
  struct Entry { uint32_t addr = 0; const char* name = ""; };
  Entry mStack[MAX_DEPTH] = {};
  int   mDepth = 0;
  int   mOverflow = 0;
};

// Declare "this native producer is drawing" for a scope. `guestAddr` is the guest submitter fn this
// producer reimplements, so its row is shared with the guest leg. `name` must be a STRING LITERAL or
// otherwise outlive the scope — it is stored by pointer, not copied (these are per-prim-hot paths).
class ProducerScope {
 public:
  ProducerScope(ProducerScopeState* st, uint32_t guestAddr, const char* name) : mSt(st) {
    if (mSt) mSt->push(guestAddr, name);
  }
  ~ProducerScope() { if (mSt) mSt->pop(); }
  ProducerScope(const ProducerScope&) = delete;
  ProducerScope& operator=(const ProducerScope&) = delete;

 private:
  ProducerScopeState* mSt;
};
