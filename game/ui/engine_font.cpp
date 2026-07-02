// PC-native engine FONT / TEXT system init — reimplementing FUN_80075130 (called from ov_game_main's
// init prefix). Per the boundary (CLAUDE.md): the front-end UI / text system is ENGINE → reimplement
// PC-native. FUN_80075130 sets a few engine-state fields directly and orchestrates 14 callees.
//
// SCOPE (deferred-libgpu carve-out, per docs/port-progress.md §A): own the ORCHESTRATION + the direct
// memsets/field writes + the 3 ENGINE-STATE callees (FUN_800963a0 font-bank select, FUN_80096370
// font-bank2, FUN_800752b4 glyph-class table fill). KEEP the 8 libgpu/libgs/sound callees as
// `rec_dispatch` IN-CONTEXT, exactly where the recomp body calls them — they do indirect draw-env /
// FntLoad/FntOpen setup and carry the later-182b nested-dispatch divergence risk, so we do NOT own them.
//
// CRITICAL stack detail: two of the KEPT (dispatched) libgpu callees (0x80098330 / 0x80098d30) read a
// struct that FUN_80075130 builds on ITS OWN STACK FRAME at sp+16. So this native orchestrator allocates
// the same `sp -= 48` frame, populates sp+16..sp+26, and passes `a0 = sp+16` to them — otherwise the
// dispatched FntOpen reads garbage. The frame is torn down (sp/ra restored) at the end (mirror epilogue).
//
// RE source: tools/disas.py 0x80075130 / 0x800963a0 / 0x80096370 / 0x800752b4 (+ --mem). Full RE table +
// per-callee semantics in docs/engine_re.md "FUN_80075130 font / text init". Store widths are exact
// (sb/sh/sw) — they are the engine-interface state the rest of the engine + retained PSX content read.
#include "core.h"
#include "ui/font.h"
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);   // run a kept (libgpu/sound) callee in-context

// FUN_800963a0 — font-bank selector. If ((bank-1)&0xff) < 24, store the bank byte at
// 0x80105cec and return the sign-extended low byte; otherwise return -1. (At the init call bank=24 →
// (24-1)&0xff = 23 < 24 → store 24, return 24.) Leaf, no sub-calls.
void Font::bankSelect(uint32_t bank) {
  Core* c = this->core;
  uint32_t v = (bank - 1) & 0xff;
  if (v < 24) {
    c->mem_w8(0x80105cecu, (uint8_t)bank);
    c->r[2] = (uint32_t)(int32_t)(int8_t)(uint8_t)bank;   // (bank<<24)>>24 : sign-extend low byte
  } else {
    c->r[2] = (uint32_t)-1;
  }
}

// FUN_80096370 — font-bank2 store. `*0x80105d28(sb) = bank; jr ra`. Leaf; does NOT set v0
// (recomp body left v0 untouched — the caller ignores it). At the init call bank=0.
void Font::bank2Store(uint32_t bank) {
  this->core->mem_w8(0x80105d28u, (uint8_t)bank);
}

// FUN_800752b4 — glyph-class table fill. Iterates i = 0..23 over a 24-entry table at base
// 0x800be238 (stride 12, write byte at entry+8). Thresholds from cls: t1=24-cls, t0=16-cls, a3=12-cls,
// a4=8-cls. The slt/bne tests branch AWAY when (i<thr) is true, so the fall-through (i>=thr) assigns:
//   i>=t1 ->4 ; i>=t0 ->1 ; i>=a3 ->3 ; i>=a4 ->2 ; else ->0   (exclusive cascade, first match wins).
// Returns the count in v0 but the caller IGNORES it. Only writes 0x800be238 + i*12 + 8 (sb).
void Font::glyphClassFill(int32_t cls) {
  Core* c = this->core;
  const uint32_t base = 0x800be238u;
  int32_t t1 = 24 - cls, t0 = 16 - cls, a3 = 12 - cls, a4 = 8 - cls;
  for (int i = 0; i < 24; i++) {
    uint8_t val;
    if      (i >= t1) val = 4;
    else if (i >= t0) val = 1;
    else if (i >= a3) val = 3;
    else if (i >= a4) val = 2;
    else              val = 0;
    c->mem_w8(base + (uint32_t)i * 12 + 8, val);
  }
  c->r[2] = 24;   // loop-exit count (caller ignores)
}

// FUN_80075130 — font / text system init orchestrator. No args, no return. Mirrors the recomp frame
// (sp -= 48; sw ra,40(sp)) because dispatched callees #11/#13 read a struct at sp+16. Owns the direct
// writes + the 3 engine callees; rec_dispatches the 8 libgpu/sound callees IN ORDER, IN-CONTEXT.
void Font::init() {
  Core* c = this->core;
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 48;
  uint32_t fsp = c->r[29];
  c->mem_w32(fsp + 40, ra);                 // sw ra,40(sp)

  // #1 sound/libgs/lib init — dispatched
  rec_dispatch(c, 0x8008e040u);

  // #2 FUN_800963a0(24) — own
  bankSelect(24);
  // #3 FUN_80096370(0) — own
  bank2Store(0);

  // #4 FUN_80098f90(0, 0xffffff) — dispatched
  c->r[4] = 0; c->r[5] = 0x00ffffffu; rec_dispatch(c, 0x80098f90u);
  // #5 FUN_80091d70(1) — dispatched
  c->r[4] = 1; rec_dispatch(c, 0x80091d70u);
  // #6 FUN_80091b50(0x800be3d8, 14, 1) — dispatched
  c->r[4] = 0x800be3d8u; c->r[5] = 14; c->r[6] = 1; rec_dispatch(c, 0x80091b50u);
  // #7 FUN_80090700(127, 127)  (a1 = a0 in the original delay slot) — dispatched
  c->r[4] = 127; c->r[5] = 127; rec_dispatch(c, 0x80090700u);
  // #8 FUN_80090980() — dispatched
  rec_dispatch(c, 0x80090980u);

  // direct: *0x800bed78 = 0 (sw)  [800751a0/9c]
  c->mem_w32(0x800bed78u, 0);
  // *0x800bed80 = -1 (sh)  — original is the DELAY SLOT of the #9 jal (v0=-1), runs before #9 body.
  c->mem_w16(0x800bed80u, 0xffff);
  // #9 FUN_800752b4(2) — own
  glyphClassFill(2);

  // direct: *0x800be358 = 0 (sw)  [once], then 14× sh 0 at 0x800be3d6 stepping -8.
  c->mem_w32(0x800be358u, 0);
  for (uint32_t addr = 0x800be3d6u, n = 0; n < 14; n++, addr -= 8)
    c->mem_w16(addr, 0);

  // stack struct consumed by the dispatched #11/#13 (FntOpen): note sp+26 stores the SAME 16384, not a
  // return value (the original `sh v0,26(sp)` runs in #10's delay slot with v0 still = 16384).
  c->mem_w32(fsp + 16, 7);
  c->mem_w32(fsp + 20, 258);
  c->mem_w16(fsp + 24, 16384);
  c->mem_w16(fsp + 26, 16384);

  // #10 FUN_80098ce0(1) — dispatched
  c->r[4] = 1; rec_dispatch(c, 0x80098ce0u);
  // #11 FUN_80098330(sp+16) — dispatched (reads the struct above)
  c->r[4] = fsp + 16; rec_dispatch(c, 0x80098330u);
  // #12 FUN_80098150(1) — dispatched
  c->r[4] = 1; rec_dispatch(c, 0x80098150u);
  // #13 FUN_80098d30(sp+16) — dispatched (reads *(sp+16))
  c->r[4] = fsp + 16; rec_dispatch(c, 0x80098d30u);
  // #14 FUN_80098db0(1, 0xffffff) — dispatched
  c->r[4] = 1; c->r[5] = 0x00ffffffu; rec_dispatch(c, 0x80098db0u);

  // direct: *0x800be22a = 0 (sb), *0x800be22b = 0 (sb)
  c->mem_w8(0x800be22au, 0);
  c->mem_w8(0x800be22bu, 0);

  // mirror epilogue: lw ra,40(sp); addiu sp,48; jr ra
  c->r[29] = sp;
  c->r[31] = ra;
}
