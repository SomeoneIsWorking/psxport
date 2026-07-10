// game/render/wide_re_gpu_putdrawenv.cpp — WIDE-RE DRAFT of libgpu PutDrawEnv (0x800815D0, CONFIRMED
// identity per docs/engine_re.md's per-frame-loop RE) and 4 of its 5 previously-unowned callees
// (0x80082240, 0x800822D8, 0x80082370, 0x80082220, 0x8008238C — the "40-line struct-pack helper"
// itself, func_80081FB0 at 0x80081FB0, turned out on close RE to be ~147 gen-C lines with a genuinely
// risky conditional tail; MAPPED but NOT drafted this session, see the note at the bottom of this
// header). Dedicated deep-RE pass per docs/fleet-workflow.md §6/§9 (both prior wide-RE waves over
// game/render/wide_re_libgpu_leaves.cpp explicitly deferred this whole cluster).
//
// WIRED (2026-07-10 verify pass, docs/fleet-workflow.md §9): PutDrawEnv + the 4 leaf builders are
// re-diffed line-by-line against generated/shard_*.c (one bug found+fixed, see BUG FIX comment
// inside func_800815D0) and installed via gpu_putdrawenv_install() -> engine_set_override_main
// (game_tomba2.cpp). func_80081FB0 stays MAPPED-not-drafted/substrate as documented below; PutDrawEnv
// reaches it via rec_dispatch, which is correct with or without a future draft of it.
//
// ------------------------------------------------------------------------------------------------
// CORRECTIONS APPLIED TO THE PRIOR WAVE'S DRAFTS (2026-07-10, this pass — fixed in
// game/render/wide_re_libgpu_leaves.cpp in the same commit, per notes-honesty): while RE'ing
// PutDrawEnv's identical boot-flag-gated hook shape, TWO real bugs surfaced in that already-drafted
// file (both confirmed twice against the raw gen-C):
//   1. `func_80080F6C` (DrawSync) AND `func_80081458` (ClearOTagR) both had an INVERTED branch
//      polarity on the boot-flag hook gate: the gen-C for both is
//      `_t = (bootFlag < 2); if (_t) goto <skip>;` — i.e. bootFlag<2 SKIPS the hook; the call
//      happens on fallthrough, when bootFlag>=2. The drafts called it when <2. PutDrawEnv below has
//      the same shape (generated/shard_1.c:15851) and is drafted with the correct >=2 polarity.
//      Consequence: the hook is a steady-state "GPU sys is up" hook, not a boot-time init.
//   2. `func_80081458`'s dummy-tail-packet constants were 0x800A5B20/0x800A5B0C; the gen-C decimals
//      (+23136/+23116 from 0x800A0000) are 0x800A5A60/0x800A5A4C — a decimal→hex slip, off by 0xC0.
// Both fixed at the source. This is fleet-workflow.md §9's bug-farm prediction landing in practice —
// a wiring pass must STILL re-diff every line of every draft in these files against the gen-C.
//
// GUEST ABI: a0(r4) = drawEnvPtr, a DRAWENV-shaped struct: +0/+2 = clip TL x/y (s16), +4/+6 = clip
// w/h (u16, x1=x0+w-1/y1=y0+h-1), +8/+10 = drawing offset x/y (s16), +12.. = texture-window struct
// (consumed by func_8008238C, see below), +20 = rgbBits (u16), +22 = dither/rgb-present flag (u8),
// +23 = tpage modeFlag (u8), +24 = "clear/background enabled" flag (u8, gates func_80081FB0's
// un-drafted tail — a FillRect(x,y,w,h,r,g,b) packet built from further drawEnvPtr fields not RE'd
// this session). Return v0 = drawEnvPtr (the same pointer passed in — matches the real SDK's
// `PutDrawEnv` returning its argument).
//
// CONTROL FLOW: (1) boot-flag-gated one-time init hook (same GPU_SYS_INIT_FN pattern as DrawSync/
// ClearOTagR, called when bootFlag>=2 — see polarity note above). (2) func_80081FB0(dst=drawEnvPtr+28,
// src=drawEnvPtr) packs the DRAWENV fields into a GP0 packet at drawEnvPtr+28 (6+ words: TL/BR/
// offset/tpage/twin/maskbit, optionally +FillRect words — MAPPED not drafted, see below). (3) ORs
// 0x00FFFFFF into *(drawEnvPtr+28) (the packet's first word — an OT-tag-style "no next" terminator
// pattern, same idiom as ClearOTagR's dummy-tail-packet). (4) Calls (*GPU_SYS_TABLE)[+0x08] (the
// "DMA-send" table slot — GPU_SYS_TABLE is a POINTER FIELD, dereferenced TWICE, same
// missing-indirection shape already found+fixed in DrawSync/ClearOTagR, see BUG FIX comment at the
// call site) with a0=(*GPU_SYS_TABLE)[+0x18] (the DrawOTag table slot's raw VALUE, passed as DATA not
// invoked — exact semantic role not confirmed this session, transcribed literally), a1=drawEnvPtr+28
// (the packed packet), a2=64, a3=0. (5) memcpy(dst=0x800A59B0, src=drawEnvPtr, 92 bytes) via
// func_8009A3E0 — caches the DRAWENV as the "current" env for later reference (classic libgpu
// PutDrawEnv semantic: remembers the last env set).
//
// MAPPED, NOT DRAFTED this session: **func_80081FB0** (0x80081FB0). RE'd its full structure from
// generated/shard_4.c:12768 (147 gen-C lines, frame -40, spills ra/s0/s1 == r16/r17):
//   - Calls the 4 leaves DRAFTED in this file below, in order: func_80082240(x0,y0)->*(dst+4)
//     [SetDrawAreaTopLeft], func_800822D8(x1,y1)->*(dst+8) [SetDrawAreaBottomRight, x1/y1 computed as
//     x0+w-1/y0+h-1], func_80082370(offX,offY)->*(dst+12) [SetDrawingOffset], func_80082220(modeFlag,
//     ditherFlag,rgbBits)->*(dst+16) [DR_TPAGE mode word — the SAME computation func_80083DE0
//     (already drafted, wide_re_libgpu_leaves.cpp) inlines for its own mode word; independently
//     re-derived and confirmed byte-identical this session], func_8008238C(texWinPtr)->*(dst+20)
//     [DR_TWIN word — SAME computation as func_80083DE0's tail; the two functions' differing
//     `(x&0xFF)>>3<<5` vs `(x<<2)&0x3E0` idioms for the Y-offset term were checked bit-for-bit and
//     proven algebraically identical over the relevant 5-bit range, cross-validating that earlier
//     draft rather than finding a bug]. Then *(dst+24) = a fixed 0xE6000000 (GP0(0xE6) SetMaskBit,
//     written with no mask bits set — a default-off placeholder, not computed from any drawEnvPtr
//     field this session located).
//   - Reads drawEnvPtr+24 (a "clear enabled" flag byte). If 0: writes the OT-tag length byte
//     *(dst+3) = 6-1 = 5 and returns (a 6-word packet: TL/BR/offset/tpage/twin/maskbit). If nonzero:
//     builds an ADDITIONAL FillRect-shaped tail (GP0(0x02)-style: color word packed from
//     drawEnvPtr+25/26/27 RGB bytes, plus 2 more words from a local sp-scratch pair) whose exact
//     packing depends on whether drawEnvPtr's clip w/h (reloaded, masked `&63`) are zero mod 64 —
//     choosing between two different word-index layouts (`L_80082134` vs `L_800821AC` in the gen-C).
//     This tail's exact field provenance for the two local sp-scratch words (sp+16/sp+20 in the gen
//     body) was NOT fully traced to named drawEnvPtr fields this session — HIGH risk of a transcribed
//     bug if drafted without further tracing (matches fleet-workflow.md §9's bug-farm shape). Left
//     MAPPED for a dedicated follow-up; PutDrawEnv itself (this file) reaches it only via
//     `rec_dispatch(c, 0x80081FB0)`, staying correct regardless of when/whether it's later drafted.
//   Confidence: HIGH on the 6-word header path (every field cross-validated against the 4 already-
//   drafted-here leaves' call sites); LOW on the FillRect tail's field semantics.
#include "core.h"
#include <stdint.h>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {
constexpr uint32_t GPU_SYS_BASE = (32778u << 16);              // 0x800A0000
constexpr uint32_t GPU_SYS_TABLE      = GPU_SYS_BASE + 22936;  // 0x800A5998
constexpr uint32_t GPU_SYS_INIT_FN    = GPU_SYS_BASE + 22940;  // 0x800A599C
constexpr uint32_t GPU_BOOT_FLAG      = GPU_SYS_BASE + 22946;  // 0x800A59A2
constexpr uint32_t GPU_CLIP_MAXW      = GPU_SYS_BASE + 22948;  // 0x800A59A4
constexpr uint32_t GPU_CLIP_MAXH      = GPU_SYS_BASE + 22950;  // 0x800A59A6
constexpr uint32_t GPU_CURRENT_ENV    = GPU_SYS_BASE + 22960;  // 0x800A59B0 (GPU_BOOT_FLAG+14)

constexpr uint32_t FN_PUTDRAWENV_PACK = 0x80081FB0u;  // MAPPED not drafted, see file header
constexpr uint32_t FN_MEMCPY_92       = 0x8009A3E0u;  // shared memcpy-like primitive, not drafted (out of band)
}  // namespace

// func_80082240 (0x80082240) — SetDrawAreaTopLeft(x,y) word builder. DRAFT. RE'd from
// generated/shard_6.c gen_func_80082240 (37 gen-C ln, no calls, no branches beyond the clip, no stack
// frame — true leaf). Guest ABI: a0(r4)=x, a1(r5)=y (both s16). Clamps each to [0, clipMax-1] against
// GPU_CLIP_MAXW/MAXH (the SAME globals func_80082734's rect clip uses — offsets 22948/22950),
// negative values clamp to 0. Returns 0xE3000000 (GP0(0xE3) SetDrawAreaTopLeft tag) |
// (clampedY&1023)<<10 | (clampedX&1023).
static void func_80082240(Core* c) {
  int32_t x = (int16_t)c->r[4];
  int32_t y = (int16_t)c->r[5];

  uint32_t clampedX;
  if (x < 0) {
    clampedX = 0;
  } else {
    int32_t maxWSigned = c->mem_r16s(GPU_CLIP_MAXW);
    uint32_t maxWUnsigned = c->mem_r16(GPU_CLIP_MAXW);
    clampedX = ((maxWSigned - 1) < x) ? (maxWUnsigned - 1u) : (uint32_t)x;
  }

  uint32_t clampedY;
  if (y < 0) {
    clampedY = 0;
  } else {
    int32_t maxHSigned = c->mem_r16s(GPU_CLIP_MAXH);
    uint32_t maxHUnsigned = c->mem_r16(GPU_CLIP_MAXH);
    clampedY = ((maxHSigned - 1) < y) ? (maxHUnsigned - 1u) : (uint32_t)y;
  }

  c->r[2] = 0xE3000000u | ((clampedY & 1023u) << 10) | (clampedX & 1023u);
}

// func_800822D8 (0x800822D8) — SetDrawAreaBottomRight(x,y) word builder. DRAFT. RE'd from
// generated/shard_7.c gen_func_800822D8 (37 gen-C ln). IDENTICAL shape/clamp logic to func_80082240
// above (same GPU_CLIP_MAXW/MAXH globals), only the tag differs: 0xE4000000 (GP0(0xE4)
// SetDrawAreaBottomRight) instead of 0xE3000000.
static void func_800822D8(Core* c) {
  int32_t x = (int16_t)c->r[4];
  int32_t y = (int16_t)c->r[5];

  uint32_t clampedX;
  if (x < 0) {
    clampedX = 0;
  } else {
    int32_t maxWSigned = c->mem_r16s(GPU_CLIP_MAXW);
    uint32_t maxWUnsigned = c->mem_r16(GPU_CLIP_MAXW);
    clampedX = ((maxWSigned - 1) < x) ? (maxWUnsigned - 1u) : (uint32_t)x;
  }

  uint32_t clampedY;
  if (y < 0) {
    clampedY = 0;
  } else {
    int32_t maxHSigned = c->mem_r16s(GPU_CLIP_MAXH);
    uint32_t maxHUnsigned = c->mem_r16(GPU_CLIP_MAXH);
    clampedY = ((maxHSigned - 1) < y) ? (maxHUnsigned - 1u) : (uint32_t)y;
  }

  c->r[2] = 0xE4000000u | ((clampedY & 1023u) << 10) | (clampedX & 1023u);
}

// func_80082370 (0x80082370) — SetDrawingOffset(x,y) word builder. DRAFT. RE'd from
// generated/shard_0.c gen_func_80082370 (9 gen-C ln, fully self-contained, no clamp — offsets are
// signed 11-bit HW fields, no range-check in the gen body). Guest ABI: a0(r4)=x, a1(r5)=y. Returns
// 0xE5000000 (GP0(0xE5) SetDrawingOffset) | (y&2047)<<11 | (x&2047).
static void func_80082370(Core* c) {
  uint32_t x = c->r[4] & 2047u;
  uint32_t y = c->r[5] & 2047u;
  c->r[2] = 0xE5000000u | (y << 11) | x;
}

// func_80082220 (0x80082220) — DR_TPAGE mode-word builder. DRAFT. RE'd from generated/shard_5.c
// gen_func_80082220 (10 gen-C ln, fully self-contained, no stack frame). This is the STANDALONE
// version of the SAME computation func_80083DE0 (already drafted, wide_re_libgpu_leaves.cpp) inlines
// for its own mode word — independently re-derived here and confirmed byte-identical, cross-
// validating that earlier draft. Guest ABI: a0(r4)=modeFlag (bool: nonzero ORs 0x400 into the low
// half), a1(r5)=ditherFlag (bool: nonzero ORs 0x200 into the tag half), a2(r6)=rgbBits (masked to
// 0x9FF). Returns 0xE1000000 [|0x200 if ditherFlag!=0] | (rgbBits&0x9FF) [|0x400 if modeFlag!=0].
// (Caller func_80081FB0 passes a0=*(drawEnv+23), a1=*(drawEnv+22), a2=*(drawEnv+20).)
static void func_80082220(Core* c) {
  uint32_t modeFlag = c->r[4];
  uint32_t ditherFlag = c->r[5];
  uint32_t rgbBits = c->r[6];

  uint32_t tagHalf = 0xE1000000u;
  if (ditherFlag != 0) tagHalf |= 0x200u;

  uint32_t lowHalf = rgbBits & 0x9FFu;
  if (modeFlag != 0) lowHalf |= 0x400u;

  c->r[2] = tagHalf | lowHalf;
}

// func_8008238C (0x8008238C) — DR_TWIN word builder. DRAFT. RE'd from generated/shard_1.c
// gen_func_8008238C (32 gen-C ln, frame -16 for 4 words of LOCAL scratch that are written but never
// re-read anywhere in the function — dead stores, mirrored here anyway for stack-byte fidelity per
// this codebase's "mirror the guest stack" rule; they are popped before return so no caller-visible
// state depends on them, but mirroring costs nothing here). Guest ABI: a0(r4)=texWinSrc, a 4-byte-
// aligned struct {u8 maskX, u8 maskY, s16 offX, s16 offY} (same shape func_80083DE0 reads). Returns 0
// if texWinSrc==0; else 0xE2000000 (GP0(0xE2) DR_TWIN) | (maskY>>3)<<15 | (maskX>>3)<<10 |
// ((-offY)&0xFF)>>3<<5 | ((-offX)&0xFF)>>3 — algebraically IDENTICAL to func_80083DE0's DR_TWIN tail
// (verified: `((x&0xFF)>>3)<<5 == (x<<2)&0x3E0` for the Y-offset term, since both formulas only
// depend on bits[3..7] of x).
static void func_8008238C(Core* c) {
  const uint32_t texWinSrc = c->r[4];
  if (texWinSrc == 0) { c->r[2] = 0; return; }

  c->r[29] -= 16;  // local scratch frame — dead stores, mirrored for stack-byte fidelity (see above)

  uint32_t maskX = c->mem_r8(texWinSrc + 0);
  uint32_t maskXShifted = maskX >> 3;
  c->mem_w32(c->r[29] + 0, maskXShifted);  // dead store

  int32_t offX = c->mem_r16s(texWinSrc + 4);
  uint32_t negOffX = (uint32_t)(0 - offX);
  negOffX &= 255u;
  negOffX = (uint32_t)((int32_t)negOffX >> 3);
  c->mem_w32(c->r[29] + 8, negOffX);  // dead store

  uint32_t maskY = c->mem_r8(texWinSrc + 2);
  uint32_t twin = (maskXShifted << 10);
  uint32_t maskYShifted = maskY >> 3;
  c->mem_w32(c->r[29] + 4, maskYShifted);  // dead store
  twin |= (maskYShifted << 15);

  int32_t offY = c->mem_r16s(texWinSrc + 6);
  twin |= 0xE2000000u;

  uint32_t negOffY = (uint32_t)(0 - offY);
  negOffY &= 255u;
  negOffY = (uint32_t)((int32_t)negOffY >> 3);
  twin |= (negOffY << 5);
  twin |= negOffX;
  c->mem_w32(c->r[29] + 12, negOffY);  // dead store

  c->r[29] += 16;
  c->r[2] = twin;
}

// func_800815D0 (0x800815D0) — libgpu PutDrawEnv(drawEnvPtr). DRAFT. RE'd from generated/shard_1.c
// gen_func_800815D0 (lines 15851-15895 in the gen-C — the shard groups an unrelated adjacent guest
// function's prologue/epilogue directly after this one's `return`, same recompiler shard-grouping
// artifact ClearOTagR's comment already flags; NOT ported here, reachable only via its own callers).
// See file header for full field-level RE, control flow, ABI, and the MAPPED-not-drafted note on
// func_80081FB0. Frame -32, spills ra/s2/s1/s0 == r18/r17/r16.
//
// REGISTER LIVENESS: unlike this band's leaf drafts, the callee-save registers here are kept LIVE in
// the guest register file (c->r[16/17/18]) across the dispatches, not shadowed in host locals — the
// dispatched SUBSTRATE callees (func_80081FB0 spills s0/s1; the table+0x08 DMA-send fn — e.g.
// GpuDmaQueueEnqueue 0x80082D04 — spills s0..s3; func_8009A3E0) spill the CALLER's s-register values
// into their own guest frames, so those registers must hold the values the real machine had at each
// call site (s2/r18 = &bootFlag global, s1/r17 = drawEnvPtr, s0/r16 = dstPacket) for the callee
// frames to byte-match. Same doctrine as faithful-execution.md's "ABI slots hold live values".
static void func_800815D0(Core* c) {
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->r[18] = GPU_BOOT_FLAG;             // s2 = &bootFlag global (0x800A59A2), live across all calls
  c->mem_w32(c->r[29] + 28, c->r[31]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[17] = c->r[4];                   // s1 = drawEnvPtr (delay-slot write, unconditional)

  auto epilogue = [&](uint32_t retVal) {
    c->r[2] = retVal;
    c->r[31] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  };

  uint8_t bootFlag = c->mem_r8(GPU_BOOT_FLAG);
  if (bootFlag >= 2) {
    // NOTE: this is the FALLTHROUGH arm of `if (bootFlag < 2) goto <skip>` in the raw gen-C — i.e.
    // the hook fires when bootFlag>=2, NOT <2. See the file header's polarity correction note.
    c->r[4] = (32770u << 16) + (uint32_t)(int32_t)(-16492);  // 0x8001BF94, fixed BIOS-window hook arg
    uint32_t initFn = c->mem_r32(GPU_SYS_INIT_FN);
    c->r[5] = c->r[17];
    c->r[31] = 0x8008161Cu;  // guest call-site return address — callees spill ra to their own frames
    rec_dispatch(c, initFn);
  }

  c->r[16] = c->r[17] + 28;  // s0 = dstPacket, live across the remaining calls
  c->r[4] = c->r[16];
  c->r[5] = c->r[17];
  c->r[31] = 0x8008162Cu;
  rec_dispatch(c, FN_PUTDRAWENV_PACK);  // func_80081FB0, MAPPED not drafted — see file header

  c->mem_w32(c->r[17] + 28, c->mem_r32(c->r[17] + 28) | 0x00FFFFFFu);

  // BUG FIX (verify pass): GPU_SYS_TABLE (0x800A5998) is a POINTER FIELD holding the real table's
  // base, not the table itself — gen dereferences it TWICE (generated/shard_1.c:15876-15878:
  // `r3=mem_r32(base+22936); r4=mem_r32(r3+24); r2=mem_r32(r3+8)`), matching the SAME missing-
  // indirection bug already found+fixed in DrawSync/ClearOTagR (wide_re_libgpu_leaves.cpp). The
  // original draft here read `mem_r32(GPU_SYS_TABLE + 24/8)` directly (single deref) — wrong.
  uint32_t tableBase = c->mem_r32(GPU_SYS_TABLE);
  uint32_t drawOTagFnValue = c->mem_r32(tableBase + 24);  // table+0x18 = DrawOTag slot's VALUE, passed as DATA
  uint32_t dmaSendFn = c->mem_r32(tableBase + 8);         // table+0x08 = DMA-send slot
  c->r[4] = drawOTagFnValue;
  c->r[5] = c->r[16];
  c->r[6] = 64u;
  c->r[7] = 0u;
  c->r[31] = 0x80081664u;
  rec_dispatch(c, dmaSendFn);

  c->r[4] = c->r[18] + 14;  // &bootFlag + 14 = GPU_CURRENT_ENV (0x800A59B0), exactly as the gen computes it
  c->r[5] = c->r[17];
  c->r[6] = 92u;
  c->r[31] = 0x80081674u;
  rec_dispatch(c, FN_MEMCPY_92);  // func_8009A3E0, out-of-band shared primitive, not drafted

  epilogue(c->r[17]);
}

// ------------------------------------------------------------------------------------------------
// WIRING (verify pass, 2026-07-10): re-diffed every line of func_800815D0 and the 4 leaf builders
// above against generated/shard_*.c per fleet-workflow.md §9. One real bug found+fixed (see the BUG
// FIX comment inside func_800815D0): GPU_SYS_TABLE double-deref, same missing-indirection shape
// already found in DrawSync/ClearOTagR (wide_re_libgpu_leaves.cpp). All 5 addresses are PLAIN
// intra-shard C calls (func_X(c), not rec_dispatch) at their call sites in generated/, so they wire
// via the oracle-gated engine_set_override_main thunk (same shape as gpu_libgpu_leaves_install) —
// this keeps SBS core B running the pure gen_func_* body.
extern void gen_func_800815D0(Core*);
extern void gen_func_80082240(Core*);
extern void gen_func_800822D8(Core*);
extern void gen_func_80082370(Core*);
extern void gen_func_80082220(Core*);
extern void gen_func_8008238C(Core*);

void gpu_putdrawenv_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x800815D0u, func_800815D0, gen_func_800815D0);
  engine_set_override_main(0x80082240u, func_80082240, gen_func_80082240);
  engine_set_override_main(0x800822D8u, func_800822D8, gen_func_800822D8);
  engine_set_override_main(0x80082370u, func_80082370, gen_func_80082370);
  engine_set_override_main(0x80082220u, func_80082220, gen_func_80082220);
  engine_set_override_main(0x8008238Cu, func_8008238C, gen_func_8008238C);
}
