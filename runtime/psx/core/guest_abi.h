// guest_abi.h — helpers for native overrides that preserve the MIPS register and stack ABI.
//
// Why this exists: a native body can hold a value the guest keeps LIVE
// in a callee-saved register (r16..r23/r30) as an ordinary C++ local instead of writing it to
// `c->r[N]`, so a nested guest call that spills its caller's callee-saved registers
// this routinely) spills STALE register content instead of the real value. Using GuestReg<N> instead
// of a C++ local makes that bug class impossible to write: the "local" IS `c->r[N]`.
//
// This header is pure stdlib + Core; no new dependencies. Deliberately NOT over-templated — three
// small pieces, each usable independently:
//   1. GuestReg<N>            — a proxy for c->r[N], usable as a drop-in local variable.
//   2. GuestFrame<Size,Count> — RAII: descends sp, spills the given registers to their RE'd offsets
//                                at construction, restores + ascends at destruction.
//
// Derive frame size and spill offsets from the authenticated executable or debugger/oracle evidence;
// generated C is not an authority.
#pragma once
#include "core.h"

// ---------------------------------------------------------------------------------------------
// 1. GuestReg<N> — proxy for c->r[N]. Held BY VALUE in a faithful body in place of a bare local,
//    e.g.:  GuestReg<16> i(c); i = 0; ... i = i + 1;
//    Every read/write goes straight through to the real register file, so any nested call that
//    depends on r16 being live sees the true value, exactly like the guest machine.
template <int N> struct GuestReg {
  Core *c;
  explicit GuestReg(Core *c_) : c(c_) {}

  operator uint32_t() const {
    return c->r[N];
  }
  GuestReg &operator=(uint32_t v) {
    c->r[N] = v;
    return *this;
  }
  GuestReg &operator=(const GuestReg &o) {
    c->r[N] = static_cast<uint32_t>(o);
    return *this;
  }

  // Convenience arithmetic — mirrors the handful of ops faithful bodies actually need; deliberately
  // not a full operator suite (add more only when a real port needs it, per "don't over-template").
  GuestReg &operator+=(uint32_t v) {
    c->r[N] += v;
    return *this;
  }
  GuestReg &operator-=(uint32_t v) {
    c->r[N] -= v;
    return *this;
  }
};

// ---------------------------------------------------------------------------------------------
// 2. GuestFrame<FrameSize, NumSpills> — contract-driven RAII stack frame.
//
//    A spill table entry is (register number, sp-relative offset), in guest program order. Construction spills
//    the CURRENT live value of each register (so callee-saved liveness set by the caller BEFORE
//    entering the frame is preserved, matching the real callee-save contract); destruction restores
//    them and ascends sp. This is the exact idiom game/render/perobj_dispatch.cpp's CmdListFrame /
//    game/world/object_table.cpp hand-write today, generalized so a new port doesn't hand-roll it.
struct GuestFrameSpill {
  int reg;    // MIPS register number (16..23, 30, or 31 for ra)
  int offset; // sp-relative byte offset recovered from the guest function's prologue
};

template <int FrameSize, int NumSpills> struct GuestFrame {
  Core *c;
  const GuestFrameSpill (&spills)[NumSpills];
  uint32_t saved[NumSpills];

  GuestFrame(Core *c_, const GuestFrameSpill (&spills_)[NumSpills]) : c(c_), spills(spills_) {
    for (int i = 0; i < NumSpills; i++) {
      saved[i] = c->r[spills[i].reg];
    }
    c->r[29] -= FrameSize;
    for (int i = 0; i < NumSpills; i++) {
      c->mem_w32(c->r[29] + (uint32_t)spills[i].offset, saved[i]);
    }
  }
  ~GuestFrame() {
    for (int i = 0; i < NumSpills; i++) {
      c->r[spills[i].reg] = c->mem_r32(c->r[29] + (uint32_t)spills[i].offset);
    }
    c->r[29] += FrameSize;
  }

  // No frame at all (frame_size == 0, e.g. a leaf with only local scratch or no sp descent) — use
  // GuestFrame<0, 0> with an empty spill table; the ctor/dtor become no-ops on sp (spill loop is
  // zero-length), so a leaf can uniformly declare a frame guard without a special case.
};

// ---------------------------------------------------------------------------------------------
// 3. guest_mult / guest_div — MIPS mult/div with the hi/lo side-effect. hi/lo are GUEST-VISIBLE
//    state (SBS compares them transitively through later spills); a faithful body that multiplies
//    where gen emits `mult` MUST go through these, never a bare C `*` (the gpuLoadImageStream
//    lesson, docs/findings/sbs.md). Returns the full product so call sites read like arithmetic:
//        vol = (int32_t)(guest_mult(c, base, cur) >> 15);
inline int64_t guest_mult(Core *c, int32_t a, int32_t b) {
  int64_t p = (int64_t)a * (int64_t)b;
  c->lo = (uint32_t)p;
  c->hi = (uint32_t)((uint64_t)p >> 32);
  return p;
}

inline void guest_div(Core *c, int32_t num, int32_t den) {
  if (den == 0) {
    return; // the translated instruction owner routes divide-by-zero through the typed fault path
  }
  c->lo = (uint32_t)(num / den);
  c->hi = (uint32_t)(num % den);
}
