// guest_abi.h — OPT-IN helpers for writing new faithful ports with the register file / guest stack
// as the ONLY place live values are held (see docs/port-framework.md, CLAUDE.md "MIRROR THE GUEST
// STACK"). Not a mass-migration: existing hand-ports keep their `c->r[N]` / manual sp descent style
// unless a session explicitly rewrites one (see docs/port-framework.md's demo migration).
//
// Why this exists: the recurring "Enqueue bug" class — a faithful body holds a value gen keeps LIVE
// in a callee-saved register (r16..r23/r30) as an ordinary C++ local instead of writing it to
// `c->r[N]`, so a NESTED call that spills its caller's callee-saved regs (still-substrate leaves do
// this routinely) spills STALE register content instead of the real value. Using GuestReg<N> instead
// of a C++ local makes that bug class impossible to write: the "local" IS `c->r[N]`.
//
// This header is pure stdlib + Core; no new dependencies. Deliberately NOT over-templated — three
// small pieces, each usable independently:
//   1. GuestReg<N>            — a proxy for c->r[N], usable as a drop-in local variable.
//   2. GuestFrame<Size,Count> — RAII: descends sp, spills the given registers to their RE'd offsets
//                                at construction, restores + ascends at destruction.
//   3. guest_call / guest_dispatch — sets the RE'd c->r[31] return-address constant, then calls.
//
// The frame's (size, spill table) contract is NOT hand-derived: run
//     python3 tools/abi_extract.py <addr> --scaffold --guestabi
// to emit a ready-to-paste `static constexpr GuestFrameSpill kSpills[] = {...}` + `GuestFrame<...>`
// declaration straight from the ground-truth generated/*.c body (see docs/abi-extract.md).
#pragma once
#include "core.h"

// ---------------------------------------------------------------------------------------------
// 1. GuestReg<N> — proxy for c->r[N]. Held BY VALUE in a faithful body in place of a bare local,
//    e.g.:  GuestReg<16> i(c); i = 0; ... i = i + 1;
//    Every read/write goes straight through to the real register file, so any nested call that
//    depends on r16 being live sees the true value, exactly like the guest machine.
template <int N>
struct GuestReg {
  Core* c;
  explicit GuestReg(Core* c_) : c(c_) {}

  operator uint32_t() const { return c->r[N]; }
  GuestReg& operator=(uint32_t v) { c->r[N] = v; return *this; }
  GuestReg& operator=(const GuestReg& o) { c->r[N] = static_cast<uint32_t>(o); return *this; }

  // Convenience arithmetic — mirrors the handful of ops faithful bodies actually need; deliberately
  // not a full operator suite (add more only when a real port needs it, per "don't over-template").
  GuestReg& operator+=(uint32_t v) { c->r[N] += v; return *this; }
  GuestReg& operator-=(uint32_t v) { c->r[N] -= v; return *this; }
};

// ---------------------------------------------------------------------------------------------
// 2. GuestFrame<FrameSize, NumSpills> — contract-driven RAII stack frame.
//
//    A spill table entry is (register number, sp-relative offset), in PROGRAM ORDER exactly as
//    `tools/abi_extract.py <addr> --contract` reports under "prologue spills". Construction spills
//    the CURRENT live value of each register (so callee-saved liveness set by the caller BEFORE
//    entering the frame is preserved, matching the real callee-save contract); destruction restores
//    them and ascends sp. This is the exact idiom game/render/perobj_dispatch.cpp's CmdListFrame /
//    game/world/object_table.cpp hand-write today, generalized so a new port doesn't hand-roll it.
struct GuestFrameSpill {
  int reg;      // MIPS register number (16..23, 30, or 31 for ra)
  int offset;   // sp-relative byte offset (matches abi_extract's `sp+N <- rM` line)
};

template <int FrameSize, int NumSpills>
struct GuestFrame {
  Core* c;
  const GuestFrameSpill (&spills)[NumSpills];
  uint32_t saved[NumSpills];

  GuestFrame(Core* c_, const GuestFrameSpill (&spills_)[NumSpills]) : c(c_), spills(spills_) {
    for (int i = 0; i < NumSpills; i++) saved[i] = c->r[spills[i].reg];
    c->r[29] -= FrameSize;
    for (int i = 0; i < NumSpills; i++) c->mem_w32(c->r[29] + (uint32_t)spills[i].offset, saved[i]);
  }
  ~GuestFrame() {
    for (int i = 0; i < NumSpills; i++)
      c->r[spills[i].reg] = c->mem_r32(c->r[29] + (uint32_t)spills[i].offset);
    c->r[29] += FrameSize;
  }

  // No frame at all (frame_size == 0, e.g. a leaf with only local scratch or no sp descent) — use
  // GuestFrame<0, 0> with an empty spill table; the ctor/dtor become no-ops on sp (spill loop is
  // zero-length), so a leaf can uniformly declare a frame guard without a special case.
};

// ---------------------------------------------------------------------------------------------
// 3. Call-site helpers — set the RE'd r31 return-address constant, then call. Mirrors the
//    `c->r[31] = 0x...u; func_XXXXXXXX(c);` / `c->r[31] = 0x...u; rec_dispatch(c, target);` idiom
//    that appears before every real call in generated/*.c (see abi_extract's "call sites" section).
void rec_dispatch(Core*, uint32_t);

inline void guest_call(Core* c, uint32_t ra_const, void (*fn)(Core*)) {
  c->r[31] = ra_const;
  fn(c);
}

inline void guest_dispatch(Core* c, uint32_t ra_const, uint32_t target) {
  c->r[31] = ra_const;
  rec_dispatch(c, target);
}

// ---------------------------------------------------------------------------------------------
// 4. guest_fn — call a guest function the way PC code calls a function. Register args land in
//    a0..a3, the RE'd jal-site constant lands in r31, the call routes through rec_dispatch, and
//    the guest's return value (v0) comes back as the C++ return value:
//
//        if (guest_fn(c, kRecordArrayInit, 0x801219EC, node, 18, tabA, tabB) != 0) return;
//
//    This is THE call shape for faithful bodies — it replaces every per-file `leaf1/leaf2/leaf3/
//    leaf4` helper and bare `c->r[4]=..; c->r[31]=..; rec_dispatch(..)` run (converged 2026-07-14,
//    USER directive: faithful ports must READ like PC game code; the ABI mechanics live here, not
//    in every port file). Args are register args only — a call that needs guest STACK args must
//    fill them via its GuestFrame (see abi_extract's call-site report).
template <typename... Args>
inline uint32_t guest_fn(Core* c, uint32_t target, uint32_t ra_const, Args... args) {
  static_assert(sizeof...(Args) <= 4, "a0..a3 only — stack args go through the GuestFrame");
  const uint32_t a[] = {static_cast<uint32_t>(args)..., 0u};   // +0 sentinel so empty packs compile
  for (size_t i = 0; i < sizeof...(Args); i++) c->r[4 + i] = a[i];
  c->r[31] = ra_const;
  rec_dispatch(c, target);
  return c->r[2];
}

// ---------------------------------------------------------------------------------------------
// 4b. guest_leaf — guest_fn WITHOUT a jal-site ra constant: args land in a0..a3 and the call
//     dispatches with r31 left as-is. This is the exact semantics of the per-file `leaf1/leaf2/
//     leaf3/leaf4` helpers it replaces (converged 2026-07-14) — kept as its own named shape, NOT
//     a guest_fn overload, so (a) the two can't be confused by argument count and (b) the missing
//     jal-site is greppable debt: when a handler gets fidelity-audited (abi_extract names the
//     call-site r31 constants), upgrade each guest_leaf to guest_fn. New ports use guest_fn.
template <typename... Args>
inline uint32_t guest_leaf(Core* c, uint32_t target, Args... args) {
  static_assert(sizeof...(Args) <= 4, "a0..a3 only — stack args go through the GuestFrame");
  const uint32_t a[] = {static_cast<uint32_t>(args)..., 0u};
  for (size_t i = 0; i < sizeof...(Args); i++) c->r[4 + i] = a[i];
  rec_dispatch(c, target);
  return c->r[2];
}

// ---------------------------------------------------------------------------------------------
// 5. guest_mult / guest_div — MIPS mult/div with the hi/lo side-effect. hi/lo are GUEST-VISIBLE
//    state (SBS compares them transitively through later spills); a faithful body that multiplies
//    where gen emits `mult` MUST go through these, never a bare C `*` (the gpuLoadImageStream
//    lesson, docs/findings/sbs.md). Returns the full product so call sites read like arithmetic:
//        vol = (int32_t)(guest_mult(c, base, cur) >> 15);
inline int64_t guest_mult(Core* c, int32_t a, int32_t b) {
  int64_t p = (int64_t)a * (int64_t)b;
  c->lo = (uint32_t)p;
  c->hi = (uint32_t)((uint64_t)p >> 32);
  return p;
}

inline void guest_div(Core* c, int32_t num, int32_t den) {   // gen `div`: lo=quot, hi=rem
  if (den == 0) return;                                       // gen guards with rec_break first
  c->lo = (uint32_t)(num / den);
  c->hi = (uint32_t)(num % den);
}
