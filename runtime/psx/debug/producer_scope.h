// producer_scope.h — which native renderer producer is pushing queue items right now (the native
// render path's identity; frame-record producers use emission_scope.h instead).
#pragma once
#include <stdint.h>

// A guest submitter address, or a PC-only producer's interned id in a separate space.
struct ProducerKey {
  enum Kind : uint8_t { NONE = 0, GUEST = 1, NATIVE_ONLY = 2 };
  Kind kind = NONE;
  uint32_t id = 0;

  static ProducerKey guest(uint32_t addr) {
    return ProducerKey{GUEST, addr};
  }
  static ProducerKey native(uint32_t iid) {
    return ProducerKey{NATIVE_ONLY, iid};
  }
  static ProducerKey none() {
    return ProducerKey{NONE, 0};
  }
  bool valid() const {
    return kind != NONE;
  }
  bool isNativeOnly() const {
    return kind == NATIVE_ONLY;
  }
  bool operator==(const ProducerKey &o) const {
    return kind == o.kind && id == o.id;
  }
};

// A PC-only producer's id: the FNV-1a hash of a stable string written at its call site.
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
