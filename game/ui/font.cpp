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
#include "game.h"           // Game::activeRq — glyphQueuePush's dual-emit target
#include "render.h"         // Render::mode.psxRender() gate
#include "render_queue.h"   // RenderQueue::push2dQuad + RQ_HUD
#include "cfg.h"            // cfg_logf fontq probe
#include "guest_abi.h"   // GuestFrame/guest_fn — ABI vocabulary (2026-07-15 readability pass)
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);   // run a kept (libgpu/sound) callee in-context

namespace {
// Font-bank engine-state bytes (own leaves FUN_800963a0/FUN_80096370).
constexpr uint32_t kFontBankAddr  = 0x80105CECu;
constexpr uint32_t kFontBank2Addr = 0x80105D28u;

// Glyph-class table (FUN_800752b4): 24 entries, stride 12, class byte at entry+8.
constexpr uint32_t kGlyphClassTableBase   = 0x800BE238u;
constexpr uint32_t kGlyphClassStride      = 12u;
constexpr uint32_t kGlyphClassFieldOffset = 8u;
constexpr int32_t  kGlyphClassCount       = 24;

// Direct engine-state fields FUN_80075130 seeds around the libgpu FntOpen block.
constexpr uint32_t kTextCursorFlagAddr = 0x800BED78u;   // sw 0                     [800751a0/9c]
constexpr uint32_t kTextUnusedFlagAddr = 0x800BED80u;   // sh -1 (jal #9 delay slot)
constexpr uint32_t kLineTableHeadAddr  = 0x800BE358u;   // sw 0 (once)
constexpr uint32_t kLineTableRowsAddr  = 0x800BE3D6u;   // 14x sh 0, stepping -8
constexpr uint32_t kLineTableRowCount  = 14u;
constexpr uint32_t kLineTableRowStride = 8u;
constexpr uint32_t kTextStateByteA     = 0x800BE22Au;   // sb 0
constexpr uint32_t kTextStateByteB     = 0x800BE22Bu;   // sb 0

// FntOpenParams — typed lens over the local FntOpen-call struct FUN_80075130 builds on its OWN
// stack frame at sp+16 (12 bytes: count u32@0, flags u32@4, size u16@8 == u16@10). The two
// dispatched libgpu callees (0x80098330/0x80098d30) read this struct by address (a0 = sp+16), so
// the frame must be real guest-stack bytes, not a native local.
struct FntOpenParams {
  Core* c;
  uint32_t base;   // == fsp + 16
  void setCount(int32_t v) { c->mem_w32(base + 0u, (uint32_t)v); }
  void setFlags(int32_t v) { c->mem_w32(base + 4u, (uint32_t)v); }
  // Both halves store the SAME value — sp+26 is NOT a computed return, the original `sh v0,26(sp)`
  // runs in the #10 call's delay slot with v0 still holding the size assigned just above.
  void setSize(uint16_t v) { c->mem_w16(base + 8u, v); c->mem_w16(base + 10u, v); }
};

// Font::init's own guest-stack frame (sp-=48; sw ra,40(sp)) — confirmed via
// `tools/abi_extract.py 0x80075130 --contract`: the ONLY prologue spill is ra at sp+40.
constexpr GuestFrameSpill kInitSpills[] = {{31, 40}};
}  // namespace

// FUN_800963a0 — font-bank selector. If ((bank-1)&0xff) < 24, store the bank byte at
// kFontBankAddr and return the sign-extended low byte; otherwise return -1. (At the init call
// bank=24 → (24-1)&0xff = 23 < 24 → store 24, return 24.) Leaf, no sub-calls (frame_size=0).
void Font::bankSelect(uint32_t bank) {
  Core* c = this->core;
  uint32_t v = (bank - 1) & 0xff;
  if (v < 24) {
    c->mem_w8(kFontBankAddr, (uint8_t)bank);
    c->r[2] = (uint32_t)(int32_t)(int8_t)(uint8_t)bank;   // (bank<<24)>>24 : sign-extend low byte
  } else {
    c->r[2] = (uint32_t)-1;
  }
}

// FUN_80096370 — font-bank2 store. `*kFontBank2Addr(sb) = bank; jr ra`. Leaf; does NOT set v0
// (recomp body left v0 untouched — the caller ignores it). At the init call bank=0.
void Font::bank2Store(uint32_t bank) {
  this->core->mem_w8(kFontBank2Addr, (uint8_t)bank);
}

// FUN_800752b4 — glyph-class table fill. Iterates i = 0..23 over the 24-entry table. Thresholds
// from cls: t1=24-cls, t0=16-cls, a3=12-cls, a4=8-cls. The slt/bne tests branch AWAY when (i<thr)
// is true, so the fall-through (i>=thr) assigns:
//   i>=t1 ->4 ; i>=t0 ->1 ; i>=a3 ->3 ; i>=a4 ->2 ; else ->0   (exclusive cascade, first match wins).
// Returns the count in v0 but the caller IGNORES it.
void Font::glyphClassFill(int32_t cls) {
  Core* c = this->core;
  int32_t t1 = 24 - cls, t0 = 16 - cls, a3 = 12 - cls, a4 = 8 - cls;
  for (int i = 0; i < kGlyphClassCount; i++) {
    uint8_t val;
    if      (i >= t1) val = 4;
    else if (i >= t0) val = 1;
    else if (i >= a3) val = 3;
    else if (i >= a4) val = 2;
    else              val = 0;
    c->mem_w8(kGlyphClassTableBase + (uint32_t)i * kGlyphClassStride + kGlyphClassFieldOffset, val);
  }
  c->r[2] = (uint32_t)kGlyphClassCount;   // loop-exit count (caller ignores)
}

// FUN_80075130 — font / text system init orchestrator. No args, no return. Mirrors the recomp frame
// (sp -= 48; sw ra,40(sp)) because dispatched callees #11/#13 read a struct at sp+16. Owns the direct
// writes + the 3 engine callees; guest_fn-dispatches the 8 libgpu/sound callees IN ORDER, IN-CONTEXT,
// using the jal-site ra constants `tools/abi_extract.py 0x80075130 --contract` reports per call site
// (single exit point — safe for GuestFrame RAII per the tail-jump gotcha in docs/faithful-execution.md).
void Font::init() {
  Core* c = this->core;
  GuestFrame<48, 1> frame(c, kInitSpills);
  uint32_t fsp = c->r[29];
  FntOpenParams fntOpen{c, fsp + 16u};

  // #1 sound/libgs/lib init — dispatched
  guest_fn(c, 0x8008e040u, 0x80075140u);

  // #2 FUN_800963a0(24) — own
  bankSelect(24);
  // #3 FUN_80096370(0) — own
  bank2Store(0);

  // #4 FUN_80098f90(0, 0xffffff) — dispatched
  guest_fn(c, 0x80098f90u, 0x80075160u, 0u, 0x00ffffffu);
  // #5 FUN_80091d70(1) — dispatched
  guest_fn(c, 0x80091d70u, 0x80075168u, 1u);
  // #6 FUN_80091b50(0x800be3d8, 14, 1) — dispatched
  guest_fn(c, 0x80091b50u, 0x8007517Cu, 0x800be3d8u, 14u, 1u);
  // #7 FUN_80090700(127, 127)  (a1 = a0 in the original delay slot) — dispatched
  guest_fn(c, 0x80090700u, 0x80075188u, 127u, 127u);
  // #8 FUN_80090980() — dispatched
  guest_fn(c, 0x80090980u, 0x80075190u);

  // direct: *kTextCursorFlagAddr = 0 (sw)  [800751a0/9c]
  c->mem_w32(kTextCursorFlagAddr, 0);
  // *kTextUnusedFlagAddr = -1 (sh) — original is the DELAY SLOT of the #9 jal (v0=-1), runs before
  // #9's body.
  c->mem_w16(kTextUnusedFlagAddr, 0xffff);
  // #9 FUN_800752b4(2) — own
  glyphClassFill(2);

  // direct: kLineTableHeadAddr = 0 (sw, once), then kLineTableRowCount x sh 0 stepping backward.
  c->mem_w32(kLineTableHeadAddr, 0);
  for (uint32_t addr = kLineTableRowsAddr, n = 0; n < kLineTableRowCount; n++, addr -= kLineTableRowStride)
    c->mem_w16(addr, 0);

  // FntOpen params consumed by the dispatched #11/#13 (they read the struct by address, a0=fsp+16).
  fntOpen.setCount(7);
  fntOpen.setFlags(258);
  fntOpen.setSize(16384);

  // #10 FUN_80098ce0(1) — dispatched
  guest_fn(c, 0x80098ce0u, 0x800751F8u, 1u);
  // #11 FUN_80098330(fsp+16) — dispatched (reads the FntOpen struct above)
  guest_fn(c, 0x80098330u, 0x80075200u, fntOpen.base);
  // #12 FUN_80098150(1) — dispatched
  guest_fn(c, 0x80098150u, 0x80075208u, 1u);
  // #13 FUN_80098d30(fsp+16) — dispatched (reads the FntOpen struct)
  guest_fn(c, 0x80098d30u, 0x80075210u, fntOpen.base);
  // #14 FUN_80098db0(1, 0xffffff) — dispatched
  guest_fn(c, 0x80098db0u, 0x80075220u, 1u, 0x00ffffffu);

  // direct: kTextStateByteA/B = 0 (sb each)
  c->mem_w8(kTextStateByteA, 0);
  c->mem_w8(kTextStateByteB, 0);

  // frame's destructor: lw ra,40(sp); addiu sp,48; jr ra
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

namespace {
// Font::drawText's guest-stack frame (sp-=32; sw ra,24(sp)) — confirmed via
// `tools/abi_extract.py 0x80079374 --contract`: the only real register spill is ra at sp+24 (the
// contract's other sp+16/sp+48 "r8" entries are the incoming/outgoing `color` stack argument, not
// a callee-save spill — handled explicitly below, same as the recomp body does).
constexpr GuestFrameSpill kDrawTextSpills[] = {{31, 24}};

constexpr uint32_t kScrGlyphAdvance = 0x1F800180u;   // per-call horizontal-advance scratch (role
                                                      // unconfirmed; glyphEmit reads it back)
}  // namespace

// FUN_80079374 — WIDE-RE TIER DRAFT (2026-07-09), UNWIRED/UNVERIFIED. See header doc for the
// full RE. Mirrors the guest frame because the callee it tail-calls (still-unowned FUN_80078CA8)
// is reached via rec_dispatch and expects the caller's stack-arg convention (5th arg at sp+16 of
// ITS caller's frame, i.e. THIS frame after the sp-=32 descent) — single exit point, safe for
// GuestFrame RAII per the tail-jump gotcha in docs/faithful-execution.md.
void Font::drawText(Core* c, int32_t x, int32_t y, int32_t w, uint32_t str, uint32_t color) {
  GuestFrame<32, 1> frame(c, kDrawTextSpills);

  // a0' = (int16)x | (y << 16) — packed vertex {x: lo16 sign-extended, y: hi16}
  uint32_t vertex = (uint32_t)(int32_t)(int16_t)(uint16_t)x | ((uint32_t)y << 16);
  // a1' = constant 0x00100008 (original a1/w argument is discarded — confirmed from the gen body)
  constexpr uint32_t kA1Const = 0x00100008u;
  // a2' = (int16)w — sign-extended low16(w) ONLY. BUG FIX (verify pass): the prior draft OR'd a
  // fabricated "h" arg into the upper 16 bits (see font.h header for the call-site trace proving
  // there is no h parameter in the real 5-arg guest ABI: x,y,w,str,color).
  uint32_t size = (uint32_t)(int32_t)(int16_t)(uint16_t)w;

  c->mem_w16(kScrGlyphAdvance, 32);         // sh v0(32),384(v1) — scratchpad write, role unconfirmed

  c->mem_w32(c->r[29] + 16, color);         // 5th arg on stack, at the callee's expected slot
  guest_fn(c, 0x80078CA8u, 0x800793B4u, vertex, kA1Const, size, str);   // FUN_80078CA8 (still unowned)
}

// ORACLE: gen_func_80079324
// FUN_80079324 — SIBLING of drawText (0x80079374): the SAME arg-packing wrapper around the same
// still-unowned emitter FUN_80078CA8, differing only in the a1 constant (0x00080008 = {w:8,h:8},
// half-height 8x8 glyphs vs drawText's 0x00100008) and the scratchpad advance value it writes to
// 0x1F800180 before the call (-32 vs drawText's +32), and the tail-call return-address constant
// (0x80079364 vs 0x800793B4). Byte-faithful to gen_func_80079324; mirrors the guest frame (sp-=32,
// ra spilled at sp+24) exactly as drawText does — single exit point, safe for GuestFrame RAII.
void Font::drawTextSmall(Core* c, int32_t x, int32_t y, int32_t w, uint32_t str, uint32_t color) {
  GuestFrame<32, 1> frame(c, kDrawTextSpills);

  // a0' = (int16)x | (y << 16) — packed vertex {x: lo16 sign-extended, y: hi16}
  uint32_t vertex = (uint32_t)(int32_t)(int16_t)(uint16_t)x | ((uint32_t)y << 16);
  // a1' = constant 0x00080008 ({w:8, h:8}) — the incoming a1/w argument is discarded, same as drawText.
  constexpr uint32_t kA1Const = 0x00080008u;
  // a2' = (int16)w — sign-extended low16(w) only.
  uint32_t size = (uint32_t)(int32_t)(int16_t)(uint16_t)w;

  // Load a0..a3 exactly as gen does (a3=str passes through the caller's r7 untouched in gen; set it
  // explicitly here — same value — matching drawText's guest_fn 4th arg).
  c->r[4] = vertex;
  c->r[5] = kA1Const;
  c->r[6] = size;
  c->r[7] = str;

  c->mem_w16(kScrGlyphAdvance, (uint16_t)-32);   // sh -32,384(v1) — scratchpad advance = -32

  c->mem_w32(c->r[29] + 16, color);         // 5th arg on stack, at the callee's expected slot
  guest_dispatch(c, 0x80079364u, 0x80078CA8u);   // ra=0x80079364; FUN_80078CA8 (still unowned)
}

// FUN_80078CA8 — the font/glyph emitter drawText() tail-calls. WIDE-RE TIER DRAFT (2026-07-10,
// disjoint band), UNWIRED/UNVERIFIED. Faithful to gen_func_80078CA8 (generated/shard_5.c:12298),
// LIVE BODY ONLY (gen-C lines 1-210; 211-402 is confirmed-unreachable dead code, no label targets
// it). See font.h for the full RE writeup (per-byte dispatch table, scratch-struct layout,
// dead-tail note). Guest-stack frame mirrored (sp-56, spill ra/s0-s5 at their RE'd offsets: r16..
// r21 = s0..s5). Kept register-literal with goto/labels named after the guest addresses (dense
// character-class branching with a shared tail reached from 5 different arms).
// Font::glyphQueuePush — the host half of glyphEmit's dual-emit (see the call site in the packet
// arm below). Reads the per-glyph scratch struct at 0x800C0000 AFTER all fields for this glyph are
// final and pushes one 2D quad to the render queue (RQ_HUD, near band), matching the guest SPRT
// byte-for-byte in placement/UV/palette: pos=+8/+10 (u16 cursor x/y), uv=+12/+13, clut=+14,
// wh=+16/+18 (drawText passes {8,16}; read live for other callers), tpage 0x1F → tp=(960,256)
// 4bpp, op 0x65 = raw (color ignored). Read-only on guest state. The struct is the SCRATCHPAD
// block at 0x1F800000 (glyphEmit's r18 = 8064<<16), not main RAM.
void Font::glyphQueuePush(Core* c) {
  if (c->game->oracle || c->rsub.mode.psxRender()) return;   // guest OT walk owns the picture
  const uint32_t st = 0x1F800000u;
  const int gx = (int16_t)c->mem_r16(st + 8u), gy = (int16_t)c->mem_r16(st + 10u);
  const int gu = c->mem_r8(st + 12u),          gv = c->mem_r8(st + 13u);
  const int gw = (int16_t)c->mem_r16(st + 16u), gh = (int16_t)c->mem_r16(st + 18u);
  const uint32_t clut = c->mem_r16(st + 14u);
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  int xs[4] = { gx + ox, gx + gw + ox, gx + ox, gx + gw + ox };
  int ys[4] = { gy + oy, gy + oy, gy + gh + oy, gy + gh + oy };
  int us[4] = { gu, gu + gw, gu, gu + gw };
  int vs[4] = { gv, gv, gv + gh, gv + gh };
  unsigned char cc[4] = { 0x80, 0x80, 0x80, 0x80 };
  { static long np = 0; if ((np++ & 127) == 0)
      cfg_logf("fontq", "push #%ld xy=(%d,%d) wh=(%d,%d) uv=(%d,%d) clut=%04X", np, gx, gy, gw, gh, gu, gv, clut); }
  c->game->activeRq().push2dQuad(RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                 /*tp_x=*/960, /*tp_y=*/256, /*mode=*/0, /*raw=*/1,
                                 (int)(clut & 0x3F) * 16, (int)(clut >> 6) & 0x1FF,
                                 0, 0, 0, 0, 0, 0, 1023, 511);
}

void Font::glyphEmit(Core* c) {
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 56u;
  c->mem_w32(c->r[29] + 44u, c->r[21]);
  c->r[21] = c->r[4] + c->r[0];             // r21 = vertex arg {x:lo16, y:hi16}
  c->mem_w32(c->r[29] + 24u, c->r[16]);
  c->r[16] = c->r[7] + c->r[0];             // r16 = str cursor (a3)
  c->mem_w32(c->r[29] + 32u, c->r[18]);
  c->r[18] = ((uint32_t)8064u << 16);       // 0x1F800000 -- glyph scratch struct base (SCRATCHPAD;
                                            // an earlier note here said 0x800C0000 — wrong, 8064=0x1F80)
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
  // DUAL-EMIT (native-render-rebuild "shared foundation — font→queue producer"): the glyph SPRT just
  // prepended as a guest packet is ALSO pushed to the native render queue, so pc_render draws text
  // (field HUD, SOP captions, attract) without transcribing the OT. Host-only: reads the completed
  // scratch struct (+8 xy, +12 uv, +14 clut, +16 wh; op 0x65 = raw textured sprite, tpage 0x1F fixed
  // by the tail's func_80083DE0 header), writes NO guest byte and NO register — the faithful body
  // above is unaffected. Gated off under psx_render/oracle where the guest OT walk draws the packet
  // itself (a push there would double-draw). Special-char icon arms (FUN_80078988) stay substrate —
  // their icons are a follow-up producer.
  glyphQueuePush(c);
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
  // sp0 IS the entry sp (saved before the -56 descent); the gen's `sp += 56` operates on the
  // DESCENDED sp. `sp0 + 56` overshot by 56 every call — the caller's frame slid up 0x38, warping
  // every subsequent packet/stack byte (MIRROR_VERIFY invocation #1: exit sp 801FE980 vs 801FE948).
  c->r[29] = sp0;
}

// ------------------------------------------------------------------------------------------------
// WIRING (verify pass, 2026-07-10, docs/fleet-workflow.md §9): drawText re-diffed line-by-line
// against generated/shard_7.c:11490 -- one real bug found+fixed (the fabricated "h" 6th argument,
// see font.h header for the full call-site trace). glyphEmit re-diffed against
// generated/shard_5.c:12298 -- byte-exact, no bugs found (also confirms the dead-tail-code claim:
// the live body's `return` at gen-C line 210 has no label past it). Both are PLAIN intra-shard C
// calls at their call sites (func_X(c), not rec_dispatch), so they wire via the oracle-gated
// engine_set_override_main thunk -- SBS core B keeps running the pure gen_func_* body.
extern void gen_func_80078988(Core*);   // icon/SJIS glyph-string leaf (iconGlyphTap runs it)
namespace {
// ov_drawText: extracts drawText's typed args from the guest ABI registers at function entry
// (a0..a2 = x,y,w; a3 = str; caller's stack[+16] = color -- matches gen_func_80079374's own read of
// sp+48 AFTER its own sp-=32, i.e. the SAME physical slot read here BEFORE any descent).
void ov_drawText(Core* c) {
  int32_t x = (int32_t)c->r[4];
  int32_t y = (int32_t)c->r[5];
  int32_t w = (int32_t)c->r[6];
  uint32_t str = c->r[7];
  uint32_t color = c->mem_r32(c->r[29] + 16u);
  Font::drawText(c, x, y, w, str, color);
}

// ov_drawTextSmall: sibling of ov_drawText for FUN_80079324 — same guest-ABI arg extraction (a0..a2
// = x,y,w; a3 = str; caller's stack[+16] = color, read BEFORE any descent).
void ov_drawTextSmall(Core* c) {
  int32_t x = (int32_t)c->r[4];
  int32_t y = (int32_t)c->r[5];
  int32_t w = (int32_t)c->r[6];
  uint32_t str = c->r[7];
  uint32_t color = c->mem_r32(c->r[29] + 16u);
  Font::drawTextSmall(c, x, y, w, str, color);
}

// iconGlyphTap — FUN_80078988, the SJIS/token ICON-GLYPH string emitter glyphEmit's 0x01..0x04
// special-char arms call (a0=x, a1=y, a2=size-class w, a3=2-byte-token string; 5th stack arg =
// OT bucket). RE from gen_func_80078988 (generated/shard_4.c:11216): second scratchpad glyph
// struct at 0x1F800020, op-0x75 8x8 sprites, clut from the size class (w<16 → row w+496 x-nibble
// 0x3F, else w+480/0x3E). Token decode per 2-byte big-endian pair:
//   0x0A0A                         -> newline (x = arg x, y += 8)
//   pair+32160 &FFFF < 26          -> code = pair+32193   (SJIS fullwidth A-Z block)
//   pair+32127 &FFFF < 26          -> code = pair+32192   (second letter block)
//   pair+32177 &FFFF < 10          -> code = pair+32193   (SJIS fullwidth digits)
//   else: token table @0x800A55E0 ({strPtr,u16 code} stride 8, 2-byte compare, NULL-terminated)
//         -> matched code, miss -> 0xFF02 (advance-only)
// Emit: glyph quad at (x,y) uv=((code&31)<<3, ((code&0xFFF)>>5)<<3), x += 8; if code&0x8000 a
// combining-mark quad at the advanced x (u = code&0x1000 ? 64 : 56, v=64) then x += 5 more.
// The tap runs gen (guest packets byte-exact) then mirrors this walk host-side into RQ_HUD quads,
// so button icons / SJIS labels render under pc_render. Same tap shape as game/ui/panel.cpp.
void iconGlyphTap(Core* c) {
  const int x0 = (int32_t)(int16_t)(uint16_t)c->r[4];
  const int y0 = (int32_t)(int16_t)(uint16_t)c->r[5];
  const int32_t wsz = (int32_t)c->r[6];
  const uint32_t str0 = c->r[7];
  gen_func_80078988(c);
  if (c->game->oracle || c->rsub.mode.psxRender()) return;   // guest OT walk owns the picture

  const uint32_t clut = (wsz < 16) ? (((uint32_t)(wsz + 496) << 6) | 63u)
                                   : (((uint32_t)(wsz + 480) << 6) | 62u);
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  auto push8 = [&](int px, int py, int u, int v) {
    int xs[4] = { px + ox, px + 8 + ox, px + ox, px + 8 + ox };
    int ys[4] = { py + oy, py + oy, py + 8 + oy, py + 8 + oy };
    int us[4] = { u, u + 8, u, u + 8 };
    int vs[4] = { v, v, v + 8, v + 8 };
    unsigned char cc[4] = { 0x80, 0x80, 0x80, 0x80 };
    c->game->activeRq().push2dQuad(RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                   960, 256, 0, /*raw=*/1,
                                   (int)(clut & 0x3F) * 16, (int)(clut >> 6) & 0x1FF,
                                   0, 0, 0, 0, 0, 0, 1023, 511);
  };
  int x = x0, y = y0;
  for (uint32_t s = str0; c->mem_r8(s) != 0; ) {
    const uint32_t pair = ((uint32_t)c->mem_r8(s) << 8) | c->mem_r8(s + 1u);
    uint32_t code;
    if (pair == 0x0A0Au)                          { s += 2; x = x0; y += 8; continue; }
    else if (((pair + 32160u) & 0xFFFFu) < 26u)   { code = (pair + 32193u) & 0xFFFFu; s += 2; }
    else if (((pair + 32127u) & 0xFFFFu) < 26u)   { code = (pair + 32192u) & 0xFFFFu; s += 2; }
    else if (((pair + 32177u) & 0xFFFFu) < 10u)   { code = (pair + 32193u) & 0xFFFFu; s += 2; }
    else {
      code = 0xFF02u;
      for (uint32_t e = 0x800A55E0u; c->mem_r32(e) != 0; e += 8u) {
        const uint32_t ts = c->mem_r32(e);
        if (c->mem_r8(ts) == c->mem_r8(s) && c->mem_r8(ts + 1u) == c->mem_r8(s + 1u)) {
          code = c->mem_r16(e + 4u);
          break;
        }
      }
      s += 2;
    }
    if (code == 0xFF02u) { x += 8; continue; }
    if (code == 0x0A0Au) { x = x0; y += 8; continue; }   // a table token can map to newline too
    push8(x, y, (code & 31u) << 3, ((code & 0xFFFu) >> 5) << 3);
    x += 8;
    if (code & 0x8000u) { push8(x, y, (code & 0x1000u) ? 64 : 56, 64); x += 5; }
  }
}
}  // namespace

extern void gen_func_80079374(Core*);
extern void gen_func_80079324(Core*);
extern void gen_func_80078CA8(Core*);
extern void gen_func_80078988(Core*);

void font_wide_re_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x80079374u, ov_drawText,      gen_func_80079374);
  engine_set_override_main(0x80079324u, ov_drawTextSmall, gen_func_80079324);   // 8x8 sibling of drawText
  engine_set_override_main(0x80078CA8u, Font::glyphEmit, gen_func_80078CA8);
  engine_set_override_main(0x80078988u, iconGlyphTap,   gen_func_80078988);   // icon/SJIS glyph strings
}
