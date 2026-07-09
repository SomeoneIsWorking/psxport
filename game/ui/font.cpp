// PC-native engine FONT / TEXT system init — reimplementing FUN_80075130 (called from game_main's
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

// FUN_80073750 — pure string measurer (disas 0x80073750..0x80073798, no sub-calls):
//   prefix = 0; suffix = 0; sawNewline = false;
//   for (ch = *p; ch != 0; ch = *++p) {
//     if (ch == '\n') sawNewline = true;
//     p++; if (sawNewline) suffix++; else prefix++;
//   }
//   NOTE: the '\n' char itself lands in `suffix` (sawNewline flips true the SAME iteration it's
//   read, and the increment below it uses the now-true flag) and, once true, sawNewline never
//   resets — a SECOND embedded '\n' is just an ordinary char counted into `suffix`.
//   if (sawNewline) return -(max(prefix, suffix));
//   else             return prefix;
int32_t Font::measureLineWidth(Core* c, uint32_t strAddr) {
  int32_t prefix = 0, suffix = 0;
  bool sawNewline = false;
  uint32_t p = strAddr;
  uint8_t ch = c->mem_r8(p);
  while (ch != 0) {
    if (ch == '\n') sawNewline = true;
    p += 1;
    if (sawNewline) suffix += 1; else prefix += 1;
    ch = c->mem_r8(p);
  }
  c->r[4] = p;   // ABI leftover: a0 ends at the NUL terminator (see header doc)
  if (sawNewline) {
    if (prefix < suffix) prefix = suffix;
    return -prefix;
  }
  return prefix;
}

// FUN_80079374 — WIDE-RE TIER DRAFT (2026-07-09), UNWIRED/UNVERIFIED. See header doc for the
// full RE. Mirrors the guest frame (sp -= 32, ra spilled at +24 — the recomp body's ONLY spill;
// the function otherwise reads args straight out of registers) because the callee it tail-calls
// (still-unowned FUN_80078CA8) is reached via rec_dispatch and expects the caller's stack-arg
// convention (5th arg at sp+16 of ITS caller's frame, i.e. THIS frame after the sp-=32 descent).
void Font::drawText(Core* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t str, uint32_t color) {
  uint32_t saved_sp = c->r[29];
  uint32_t saved_ra = c->r[31];

  c->r[29] = saved_sp - 32;                 // addiu sp,sp,-32
  c->mem_w32(c->r[29] + 24, saved_ra);      // sw ra,24(sp)  (LIVE incoming ra)

  // a0' = (int16)x | (y << 16) — packed vertex {x: lo16 sign-extended, y: hi16}
  uint32_t a0p = (uint32_t)(int32_t)(int16_t)(uint16_t)x | ((uint32_t)y << 16);
  // a1' = constant 0x00100008 (original a1/w argument is discarded — confirmed from the gen body)
  uint32_t a1p = 0x00100008u;
  // a2' = (int16)w | (h << 16) — packed size {w: lo16 sign-extended, h: hi16}
  uint32_t a2p = (uint32_t)(int32_t)(int16_t)(uint16_t)w | ((uint32_t)h << 16);

  c->mem_w16(0x1F800180u, 32);              // sh v0(32),384(v1) — scratchpad write, role unconfirmed

  c->r[31] = 0x800793B4u;                   // jal-site ra (matches gen exactly)
  c->r[4]  = a0p;
  c->r[5]  = a1p;
  c->r[6]  = a2p;
  c->r[7]  = str;
  c->mem_w32(c->r[29] + 16, color);         // 5th arg on stack, at the callee's expected slot
  rec_dispatch(c, 0x80078CA8u);             // FUN_80078CA8 — font/glyph emitter (still unowned)

  c->r[31] = c->mem_r32(c->r[29] + 24);     // lw ra,24(sp)
  c->r[29] = saved_sp;                      // addiu sp,sp,32
}

// FUN_80078CA8 — the font/glyph emitter drawText() tail-calls. WIDE-RE TIER DRAFT (2026-07-10,
// disjoint band), UNWIRED/UNVERIFIED. Faithful to gen_func_80078CA8 (generated/shard_5.c:12298),
// LIVE BODY ONLY (gen-C lines 1-210; 211-402 is confirmed-unreachable dead code, no label targets
// it). See font.h for the full RE writeup (per-byte dispatch table, scratch-struct layout,
// dead-tail note). Guest-stack frame mirrored (sp-56, spill ra/s0-s5 at their RE'd offsets: r16..
// r21 = s0..s5). Kept register-literal with goto/labels named after the guest addresses (dense
// character-class branching with a shared tail reached from 5 different arms).
void Font::glyphEmit(Core* c) {
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 56u;
  c->mem_w32(c->r[29] + 44u, c->r[21]);
  c->r[21] = c->r[4] + c->r[0];             // r21 = vertex arg {x:lo16, y:hi16}
  c->mem_w32(c->r[29] + 24u, c->r[16]);
  c->r[16] = c->r[7] + c->r[0];             // r16 = str cursor (a3)
  c->mem_w32(c->r[29] + 32u, c->r[18]);
  c->r[18] = ((uint32_t)8064u << 16);       // 0x800C0000 -- fixed scratch struct base (NOT scratchpad)
  c->r[3] = c->r[6] + c->r[0];              // r3 = size arg {w:lo16, h:hi16} (a2)
  c->r[2] = c->r[6] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);   // sign-extended low16(size) = w
  c->mem_w32(c->r[29] + 36u, c->r[19]);
  c->r[19] = c->mem_r32(c->r[29] + 72u);    // 5th arg (color), caller's stack[+16]
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 16);
  c->mem_w32(c->r[29] + 48u, c->r[31]);
  c->mem_w32(c->r[29] + 40u, c->r[20]);
  c->mem_w32(c->r[29] + 28u, c->r[17]);
  c->mem_w32(c->r[18] + 8u, c->r[21]);      // struct+8 (cursor-x u16 slot, written as u32 here -- low16 is x)
  {
    int _t = (c->r[2] != c->r[0]);
    c->mem_w32(c->r[18] + 16u, c->r[5]);    // struct+16 = a1 (drawText's 0x00100008 constant)
    if (_t) goto L_80078D04;
  }
  c->r[2] = c->r[6] + 480u;
  c->r[2] = c->r[2] << 6;
  c->r[2] = c->r[2] | 62u;
  goto L_80078D10;
L_80078D04:
  c->r[2] = c->r[6] + 496u;
  c->r[2] = c->r[2] << 6;
  c->r[2] = c->r[2] | 63u;
L_80078D10:
  c->mem_w16(c->r[18] + 14u, (uint16_t)c->r[2]);
  c->r[2] = c->r[0] + 101u;
  c->mem_w8(c->r[18] + 7u, (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 0u);
  {
    int _t = (c->r[2] == c->r[0]);
    c->r[17] = c->r[3] << 16;
    if (_t) goto L_80078F88;                // empty string -- straight to tail
  }
  c->r[20] = 0x1F800000u;                   // scratchpad base (r20 is reused as scratchpad base here)
  c->r[3] = c->r[2] & 255u;
L_80078D34:
  c->r[2] = c->r[0] + 32u;
  {
    int _t = (c->r[3] != c->r[2]);
    c->r[2] = c->r[0] + 10u;                // delay-slot literal, live at L_80078D4C
    if (_t) goto L_80078D4C;
  }
  // byte == 0x20 (' ') -- advance-cursor tail only
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[16] = c->r[16] + 1u;
  goto L_80078F70;
L_80078D4C:
  {
    int _t = (c->r[3] != c->r[2]);          // r2 == 10 here (delay-slot literal from above)
    c->r[2] = c->r[0] + 1u;                 // delay-slot literal, live at L_80078D74
    if (_t) goto L_80078D74;
  }
  // byte == 0x0A ('\n') -- line break: reset x, y += "line height" (struct+18)
  c->r[16] = c->r[16] + 1u;
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 10u);
  c->r[4] = (uint32_t)c->mem_r16(c->r[18] + 18u);
  c->r[3] = c->r[21] & 4095u;
  c->mem_w16(c->r[18] + 8u, (uint16_t)c->r[3]);
  c->r[2] = c->r[2] + c->r[4];
  c->mem_w16(c->r[18] + 10u, (uint16_t)c->r[2]);
  goto L_80078F78;
L_80078D74:
  {
    int _t = (c->r[3] != c->r[2]);          // r2 == 1 here
    c->r[2] = c->r[0] + 2u;                 // delay-slot literal, live at L_80078DB4
    if (_t) goto L_80078DB4;
  }
  // byte == 0x01 -- FUN_80078988(cursorX, cursorY, w, tablePtr=0x80010000+28072)
  c->r[6] = (uint32_t)((int32_t)c->r[17] >> 16);
  c->r[7] = ((uint32_t)32769u << 16);
  c->r[4] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[7] = c->r[7] + 28072u;
  c->mem_w32(c->r[29] + 16u, c->r[19]);
  c->r[5] = (uint32_t)c->mem_r16(c->r[18] + 10u);
  c->r[4] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
  c->r[5] = c->r[5] << 16;
  c->r[31] = 0x80078DA8u;
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
  rec_dispatch(c, 0x80078988u);             // FUN_80078988 -- still unowned, out of this wave's band
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[16] = c->r[16] + 1u;
  goto L_80078F70;
L_80078DB4:
  {
    int _t = (c->r[3] != c->r[2]);          // r2 == 2 here
    c->r[2] = c->r[0] + 3u;                 // delay-slot literal, live at L_80078DF4
    if (_t) goto L_80078DF4;
  }
  // byte == 0x02 -- FUN_80078988(cursorX, cursorY, w, tablePtr=0x80010000+28076)
  c->r[6] = (uint32_t)((int32_t)c->r[17] >> 16);
  c->r[7] = ((uint32_t)32769u << 16);
  c->r[4] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[7] = c->r[7] + 28076u;
  c->mem_w32(c->r[29] + 16u, c->r[19]);
  c->r[5] = (uint32_t)c->mem_r16(c->r[18] + 10u);
  c->r[4] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
  c->r[5] = c->r[5] << 16;
  c->r[31] = 0x80078DE8u;
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
  rec_dispatch(c, 0x80078988u);
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[16] = c->r[16] + 1u;
  goto L_80078F70;
L_80078DF4:
  {
    int _t = (c->r[3] != c->r[2]);          // r2 == 3 here
    c->r[2] = c->r[0] + 4u;                 // delay-slot literal, live at L_80078E34
    if (_t) goto L_80078E34;
  }
  // byte == 0x03 -- FUN_80078988(cursorX, cursorY, w, tablePtr=0x80010000+28068)
  c->r[6] = (uint32_t)((int32_t)c->r[17] >> 16);
  c->r[7] = ((uint32_t)32769u << 16);
  c->r[4] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[7] = c->r[7] + 28068u;
  c->mem_w32(c->r[29] + 16u, c->r[19]);
  c->r[5] = (uint32_t)c->mem_r16(c->r[18] + 10u);
  c->r[4] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
  c->r[5] = c->r[5] << 16;
  c->r[31] = 0x80078E28u;
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
  rec_dispatch(c, 0x80078988u);
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[16] = c->r[16] + 1u;
  goto L_80078F70;
L_80078E34:
  {
    int _t = (c->r[3] != c->r[2]);          // r2 == 4 here
    c->r[6] = (uint32_t)((int32_t)c->r[17] >> 16);   // delay-slot, live at L_80078E70 too
    if (_t) goto L_80078E70;
  }
  // byte == 0x04 -- FUN_80078988(cursorX, cursorY, w, tablePtr=0x80010000+28064)
  c->r[7] = ((uint32_t)32769u << 16);
  c->r[4] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[7] = c->r[7] + 28064u;
  c->mem_w32(c->r[29] + 16u, c->r[19]);
  c->r[5] = (uint32_t)c->mem_r16(c->r[18] + 10u);
  c->r[4] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 16);
  c->r[5] = c->r[5] << 16;
  c->r[31] = 0x80078E64u;
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 16);
  rec_dispatch(c, 0x80078988u);
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[16] = c->r[16] + 1u;
  goto L_80078F70;
L_80078E70:
  // default arm -- ordinary glyph: compute per-glyph width/height, prepend GP0 packet at the pool.
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 0u);
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[20] + 384u);   // scratchpad 0x1F800180 -- advance value
  c->r[4] = c->r[3] + c->r[2];
  {
    int _t = ((int32_t)c->r[4] >= 0);
    c->r[3] = c->r[4] + c->r[0];
    if (_t) goto L_80078E8C;
  }
  c->r[3] = c->r[4] + 31u;
L_80078E8C:
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 5);
  c->r[3] = c->r[3] << 5;
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 16u);
  c->r[3] = c->r[4] - c->r[3];
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[8] = c->lo;
  c->mem_w8(c->r[18] + 12u, (uint8_t)c->r[8]);
  c->r[3] = (uint32_t)c->mem_r8(c->r[16] + 0u);
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[20] + 384u);
  c->r[3] = c->r[3] + c->r[2];
  {
    int _t = ((int32_t)c->r[3] >= 0);
    if (_t) goto L_80078ECC;
  }
  c->r[3] = c->r[3] + 31u;
L_80078ECC:
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 18u);
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 5);
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[3] = (uint32_t)c->mem_r16(c->r[18] + 18u);
  c->r[2] = c->r[0] + 16u;
  c->r[3] = c->r[3] << 16;
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
  c->r[4] = c->lo;
  {
    int _t = (c->r[3] != c->r[2]);
    c->mem_w8(c->r[18] + 13u, (uint8_t)c->r[4]);
    if (_t) goto L_80078F04;
  }
  c->r[2] = c->r[4] + 8u;
  c->mem_w8(c->r[18] + 13u, (uint8_t)c->r[2]);
L_80078F04:
  // prepend a 4-word GP0 packet at the packet-pool bump pointer (PKT_POOL_PTR / 0x800BF544),
  // link it into the OT bucket for this colorArg, exactly the pattern other render leaves use.
  c->r[6] = 0x800C0000u - 2748u;            // 32780u<<16 - 2748 == PKT_POOL_PTR (0x800BF544)
  c->r[4] = c->mem_r32(c->r[6] + 0u);
  c->r[2] = 0x800F0000u;                    // 32783u<<16
  c->r[5] = c->mem_r32(c->r[2] - 10040u);   // OT-slot table base
  c->r[2] = c->r[19] << 2;
  c->r[5] = c->r[5] + c->r[2];
  c->r[2] = c->mem_r32(c->r[5] + 0u);
  c->r[3] = ((uint32_t)1024u << 16);
  c->r[2] = c->r[2] | c->r[3];
  c->mem_w32(c->r[4] + 0u, c->r[2]);
  c->mem_w32(c->r[5] + 0u, c->r[4]);
  c->r[4] = c->r[4] + 4u;
  c->r[2] = c->mem_r32(c->r[18] + 4u);
  c->r[16] = c->r[16] + 1u;
  c->mem_w32(c->r[4] + 0u, c->r[2]);
  c->r[2] = c->mem_r32(c->r[18] + 8u);
  c->r[4] = c->r[4] + 4u;
  c->mem_w32(c->r[4] + 0u, c->r[2]);
  c->r[2] = c->mem_r32(c->r[18] + 12u);
  c->r[4] = c->r[4] + 4u;
  c->mem_w32(c->r[4] + 0u, c->r[2]);
  c->r[2] = c->mem_r32(c->r[18] + 16u);
  c->r[4] = c->r[4] + 4u;
  c->mem_w32(c->r[4] + 0u, c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[4] = c->r[4] + 4u;
  c->mem_w32(c->r[6] + 0u, c->r[4]);        // advance PKT_POOL_PTR
L_80078F70:
  c->r[2] = c->r[2] + 8u;
  c->mem_w16(c->r[18] + 8u, (uint16_t)c->r[2]);
L_80078F78:
  c->r[2] = (uint32_t)c->mem_r8(c->r[16] + 0u);
  {
    int _t = (c->r[2] != c->r[0]);
    c->r[3] = c->r[2] & 255u;
    if (_t) goto L_80078D34;
  }
L_80078F88:
  // tail: final OT-chained packet via the already-owned func_80083DE0 (draw-mode/texwin header).
  c->r[5] = c->r[0] + c->r[0];
  c->r[6] = c->r[5] + c->r[0];
  c->r[17] = 0x800C0000u - 2748u;           // PKT_POOL_PTR
  c->r[16] = c->mem_r32(c->r[17] + 0u);
  c->r[7] = c->r[0] + 31u;
  c->mem_w32(c->r[29] + 16u, c->r[0]);
  c->r[31] = 0x80078FA8u;
  c->r[4] = c->r[16] + c->r[0];
  rec_dispatch(c, 0x80083DE0u);             // func_80083DE0 -- already owned, process-globally wired
  c->r[2] = 0x800F0000u;
  c->r[4] = c->mem_r32(c->r[2] - 10040u);
  c->r[2] = c->r[19] << 2;
  c->r[4] = c->r[4] + c->r[2];
  c->r[2] = c->mem_r32(c->r[4] + 0u);
  c->r[3] = ((uint32_t)512u << 16);
  c->r[2] = c->r[2] | c->r[3];
  c->mem_w32(c->r[16] + 0u, c->r[2]);
  c->mem_w32(c->r[4] + 0u, c->r[16]);
  c->r[3] = c->mem_r32(c->r[17] + 0u);
  c->r[3] = c->r[3] + 12u;
  c->mem_w32(c->r[17] + 0u, c->r[3]);       // advance PKT_POOL_PTR
  c->r[3] = c->r[21] & 65535u;
  c->r[2] = (uint32_t)c->mem_r16(c->r[18] + 8u);
  c->r[31] = c->mem_r32(c->r[29] + 48u);
  c->r[21] = c->mem_r32(c->r[29] + 44u);
  c->r[20] = c->mem_r32(c->r[29] + 40u);
  c->r[19] = c->mem_r32(c->r[29] + 36u);
  c->r[18] = c->mem_r32(c->r[29] + 32u);
  c->r[17] = c->mem_r32(c->r[29] + 28u);
  c->r[16] = c->mem_r32(c->r[29] + 24u);
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[2] = c->r[2] - c->r[3];              // return value -- caller (drawText) discards it
  c->r[29] = sp0 + 56u;
}
