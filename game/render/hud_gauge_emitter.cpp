// game/render/hud_gauge_emitter.cpp — native port of the HUD gauge emitter:
//   FUN_8004FD30 (frame entry, HudGaugeEmitter::emitFrame) +
//   FUN_8004FB4C (per-item leaf, HudGaugeEmitter::emitItem).
//
// RE: ground truth = generated/shard_0.c gen_func_8004FD30 (~line 7179) and generated/shard_7.c
// gen_func_8004FB4C (~line 6581) — the recompiler's per-instruction transcription, traced
// instruction-by-instruction and cross-checked against the Ghidra decomp
// (scratch/decomp/otattr_subs.c / otattr_leaf.c) and `tools/abi_extract.py <addr> --contract`.
// Census note: docs/findings/render.md "0x8003F9A8 474-prim attribution resolved" port order
// item (2) — "self-contained HUD gauge emitter, loop identity = 0x800BF548 stride-0x8C index".
//
// What it draws: a fixed 40x40-ish HUD gauge table at 0x800BF548 (flag/count header + an
// embedded array of 0x8C-byte item records starting at +12). Each frame, IF the table's enable
// flag is set: (1) a full-viewport DR_AREA scissor reset (320x240 at the camera's current
// vertical scroll), (2) one DR_AREA + two segment-layout calls per active item (the individual
// gauge/HP segments), (3) a small status-panel DR_AREA scissor (288x54, offset (16, scroll+153))
// — this matches the task's description of "the HP/status gauge segments".
//
// Same faithful-substrate-mirror carve-out as WidescreenMarginQuad/OverlayGt3Gt4: this is the
// SUBSTRATE's own packet-pool + OT writer, not pc_render. Every guest write below is part of the
// byte-exact state SBS compares — no write here is optional or "residual". The three still-
// substrate leaves this calls into (FUN_80081CF8 DR_AREA packet builder, FUN_8004EB94 segment
// layout, FUN_8005019C digit/label draw) stay un-owned; calls route through `guest_fn` exactly
// like gen's own `c->r[31] = <jal-site>; func_XXXXXXXX(c);` idiom, so SBS sees byte-identical
// substrate execution underneath.
#include "core.h"
#include "game.h"
#include "guest_abi.h"
#include "hud_gauge_emitter.h"
#include "render.h"          // Render::mode.psxRender() — gaugeTextRowTap's read-only overlay gate
#include "render_queue.h"    // RenderQueue::push2dQuad + RQ_HUD — the tap's host half
#include "cfg.h"             // cfg_logf gaugeq probe
#include <cstdint>

extern void gen_func_8004EB94(Core*);   // guest text-row leaf body (see gaugeTextRowTap below)

namespace {

// -------------------------------------------------------------------------------------------
// Guest addresses / constants (named, not inline hex — CLAUDE.md "no magic constant offsets").
constexpr uint32_t kHudGaugeBase   = 0x800BF548u; // DAT_800bf548: the gauge table header + array
constexpr uint32_t kFlagOff        = 1u;          // DAT_800bf549: draw-enable flag byte (==1 to draw)
constexpr uint32_t kCountOff       = 8u;          // DAT_800bf550: active-item count (signed s16)
constexpr uint32_t kRecordsOff     = 12u;          // first record = kHudGaugeBase + kRecordsOff
constexpr uint32_t kRecordStride   = 140u;         // 0x8C, per docs/findings/render.md census note

constexpr uint32_t kPktPoolBaseReg = 0x800C0000u; // 32780<<16 — the "packet-pool base" register gen
                                                   // loads (r20) and derives kPktPoolPtr from as
                                                   // base-2748; held live across the item's nested
                                                   // leaves and spilled by FUN_8005019C.
constexpr uint32_t kPktPoolPtr     = 0x800BF544u; // packet-pool bump-allocator cursor — SAME pool
                                                   // WidescreenMarginQuad/OverlayGt3Gt4 write into.
constexpr uint32_t kOtBaseReg      = 0x800F0000u; // 32783<<16 — gen's r19; kOtBase == this - 0x2738.
                                                   // Held live (spilled by FUN_8004EB94/FUN_8005019C).
constexpr uint32_t kOtBase         = 0x800ED8C8u; // guest word holding the live OT array base
                                                   // pointer — SAME address WidescreenMarginQuad
                                                   // reads as `otBase` for its Z-bucketed insert.
constexpr uint32_t kHudOtBucketIndex = 3u;         // this emitter always tail-appends into the
                                                   // FIXED near/HUD bucket otBase[3] (word offset
                                                   // +12), never a Z-derived index — traced as
                                                   // `mem32(mem32(kOtBase) + 12)` at every link site.
constexpr uint32_t kDrawAreaOtTag  = 0x02000000u; // OT tag: 2 data words follow the tag (DR_AREA's
                                                   // top-left + bottom-right words) — same len<<24
                                                   // encoding as WidescreenMarginQuad's kPktTag.
constexpr uint32_t kDrawAreaPacketBytes = 12u;    // tag word + 2 data words = 3 words = 12 bytes;
                                                   // matches FUN_80081CF8 setting its length byte to 2.

constexpr uint32_t kFunBuildDrawArea = 0x80081CF8u; // FUN_80081cf8(pkt, ushort rect[4]{x,y,w,h}):
                                                   // still-substrate DR_AREA packet-header builder
                                                   // (SetDrawAreaTopLeft/BottomRight word builders
                                                   // at 0x80082240/0x800822D8 are already owned —
                                                   // see game/render/wide_re_gpu_putdrawenv.cpp —
                                                   // but the packet-header assembly leaf itself
                                                   // isn't; called exactly as gen calls it).
constexpr uint32_t kFunSegmentLayout = 0x8004EB94u; // FUN_8004eb94(descAddr, signed16 span): still-
                                                   // substrate per-segment layout leaf.
constexpr uint32_t kFunLabelOrDigits = 0x8005019Cu; // FUN_8005019c(rectAddr, byte, 0, 3): still-
                                                   // substrate digit/label draw leaf.

// jal-site return-address constants, one per call site, in program order (mirrors gen exactly —
// FUN_80081CF8's own callee-saved footprint spills r16/r17/r31, so a wrong/garbage r31 here would
// surface as a genuine SBS diff in that leaf's own stack spill).
constexpr uint32_t kRaFrameTileA   = 0x8004FDA8u;
constexpr uint32_t kRaFrameCallItem = 0x8004FDE4u;
constexpr uint32_t kRaFrameTileB   = 0x8004FE4Cu;
constexpr uint32_t kRaItemTileA    = 0x8004FC08u;
constexpr uint32_t kRaItemSeg1     = 0x8004FC44u;
constexpr uint32_t kRaItemSeg2     = 0x8004FC74u;
constexpr uint32_t kRaItemTileB    = 0x8004FCB8u;
constexpr uint32_t kRaItemSegElse  = 0x8004FCF4u;
constexpr uint32_t kRaItemFinal    = 0x8004FD08u;

// Full-viewport scissor reset (FD30, before the item loop) — dims match the task's "0x140x0xf0".
constexpr uint16_t kViewportX = 0, kViewportW = 0x140, kViewportH = 0xF0;
// Status-panel scissor (FD30, after the item loop) — dims match the task's "0x120x0x36"; the
// panel sits inset from the viewport by (16px, +153 vertical scanlines).
constexpr uint16_t kPanelX = 0x10, kPanelW = 0x120, kPanelH = 0x36;
constexpr uint16_t kPanelYBias = 0x99; // 153

// Item record byte offsets within the 0x8C-stride array (see HudGaugeItemRecord below).
constexpr uint32_t kSegPrimaryOff   = 16u; // 0x10 — first segment-layout leaf's descriptor arg
constexpr uint32_t kSegSecondaryOff = 61u; // 0x3D — second segment-layout leaf's descriptor arg
constexpr uint32_t kLabelByteOff    = 136u; // 0x88 — byte forwarded to the digit/label leaf

// -------------------------------------------------------------------------------------------
// Scratchpad camera/vertical-scroll byte (DAT_1f800135) — read at every DR_AREA rect this
// emitter builds; always packed as the rect's Y in an 8.8-style fixed point (byte << 8).
uint16_t hudViewportY(Core* c) { return (uint16_t)(c->mem_r8(0x1F800135u) << 8); }

// Item record lens: 0x8C-byte-stride gauge-item record (see docs/findings/render.md census
// note). Field roles below are exactly what gen reads at each offset, traced instruction-by-
// instruction against gen_func_8004FB4C.
struct HudGaugeItemRecord {
  Core* c;
  uint32_t addr;

  uint8_t kind()      const { return c->mem_r8(addr + 10); }  // 0/>=3 = single-segment; 1/2 = also
                                                                // draws its own item-box + tile A.
  uint16_t spanBase()  const { return c->mem_r16(addr + 2); }  // segment span base
  uint8_t  spanBias()  const { return c->mem_r8(addr + 11); }  // segment span bias byte, added in
  uint8_t  labelByte() const { return c->mem_r8(addr + kLabelByteOff); }
};

// -------------------------------------------------------------------------------------------
// Build the {x,y,w,h} ushort rect FUN_80081CF8 reads as its param_2, at sp+off..off+6.
void buildDrawAreaRect(Core* c, uint32_t spOff, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint32_t sp = c->r[29];
  c->mem_w16(sp + spOff + 0, x);
  c->mem_w16(sp + spOff + 2, y);
  c->mem_w16(sp + spOff + 4, w);
  c->mem_w16(sp + spOff + 6, h);
}

// Emit the DR_AREA packet built from the sp+rectOff rect into the packet pool, then tail-append
// it into the fixed HUD OT bucket. GuestReg<16> mirrors gen's own r16 = old-pool-cursor
// assignment (FUN_80081CF8's callee-saved footprint spills r16/r17 — a wrong live value there is
// a real SBS diff, not residual scratch: CLAUDE.md "MIRROR THE GUEST STACK").
void emitDrawAreaAndLink(Core* c, uint32_t raConst, uint32_t rectOff) {
  GuestReg<16> pktAddr(c);
  pktAddr = c->mem_r32(kPktPoolPtr);
  c->mem_w32(kPktPoolPtr, (uint32_t)pktAddr + kDrawAreaPacketBytes);
  guest_fn(c, kFunBuildDrawArea, raConst, (uint32_t)pktAddr, c->r[29] + rectOff);

  const uint32_t otBase = c->mem_r32(kOtBase);
  const uint32_t tailSlot = otBase + kHudOtBucketIndex * 4u;
  const uint32_t oldTail = c->mem_r32(tailSlot);
  // gen holds the DR_AREA OT tag constant (512<<16 == 0x02000000) LIVE in r3/v1 across the link
  // and leaves it there — the leaf's v1 return residue. Mirror it in c->r[3] (not just an inline
  // literal) so a wrong live r3 can't leak out as this leaf's v1 (SBS core A / pc_skip=false
  // MIRROR_VERIFY caught native leaving 0x09000000 — the last nested leaf's r3 — instead).
  c->r[3] = kDrawAreaOtTag;
  const uint32_t tagWord = oldTail | c->r[3];
  c->mem_w32((uint32_t)pktAddr + 0, tagWord);
  c->mem_w32(tailSlot, (uint32_t)pktAddr);
  // gen computes this tag word in r2 as its literal last step before falling through to the
  // caller's epilogue — a real v0 return-register residue at both of emitFrame's call sites
  // (MIRROR_VERIFY caught this: without it, v0 held stale garbage from unrelated earlier code).
  c->r[2] = tagWord;
}

// FUN_8004eb94(descAddr, sign_extend16(spanBase + spanBias + bias)) call shape, shared by all
// three segment-layout call sites (primary/secondary/else).
void emitSegmentLayout(Core* c, uint32_t raConst, uint32_t descAddr,
                        const HudGaugeItemRecord& rec, int32_t bias) {
  const uint16_t sum = (uint16_t)(rec.spanBase() + rec.spanBias() + bias);
  guest_fn(c, kFunSegmentLayout, raConst, descAddr, (uint32_t)(int32_t)(int16_t)sum);
}

} // namespace

void HudGaugeEmitter::emitFrame(Core* c) {
  // Frame: sp -= 40, spill s0..s2/ra at their RE'd offsets (tools/abi_extract.py --contract).
  static constexpr GuestFrameSpill kSpills[] = { {18, 32}, {31, 36}, {17, 28}, {16, 24} };
  GuestFrame<40, 4> frame(c, kSpills);
  constexpr uint32_t kRectOff = 16; // sp+16..22, reused for both DR_AREA rects this leaf builds

  GuestReg<18> recordsBase(c); recordsBase = kHudGaugeBase; // callee-saved footprint mirror

  const int32_t count = c->mem_r16s(kHudGaugeBase + kCountOff);
  // gen's `count == 0` branch fires with r2 = 1 already live in its delay slot (`c->r[2] =
  // c->r[0] + 1` executes unconditionally right before the branch) — a v0 residue on this
  // early-return path, mirrored explicitly (MIRROR_VERIFY gates this).
  if (count == 0) { c->r[2] = 1; return; }

  if (c->mem_r8(kHudGaugeBase + kFlagOff) == 1) {
    buildDrawAreaRect(c, kRectOff, kViewportX, hudViewportY(c), kViewportW, kViewportH);
    emitDrawAreaAndLink(c, kRaFrameTileA, kRectOff);
  }

  if (count > 0) {
    GuestReg<16> loopIdx(c);
    GuestReg<17> byteOff(c);
    byteOff = kRecordsOff;
    for (loopIdx = 0; (int32_t)loopIdx < count;) {
      // Live callee-saved regs at the jal-site (tools/abi_extract.py 0x8004FD30 --contract call
      // [1]): r16=loop index, r17=byte offset, r18=table base — all three already mirrored
      // above/here, so emitItem()'s own GuestFrame spills the correct live values, not garbage.
      c->r[4] = (uint32_t)recordsBase + (uint32_t)byteOff;
      guest_call(c, kRaFrameCallItem, &HudGaugeEmitter::emitItem);
      loopIdx = (uint32_t)loopIdx + 1;
      byteOff = (uint32_t)byteOff + kRecordStride;
    }
  }

  // gen (L_8004FDF8) reloads the flag byte into r3 and r2 = 1 here, regardless of loop outcome.
  // When flag != 1 it exits immediately, leaving r3 = flag as the leaf's v1 residue and r2 = 1 as
  // v0 (SBS core A MIRROR_VERIFY caught native leaving a nested leaf's r3=0x09000000 on a flag==2
  // item). When flag == 1 it draws tile B and emitDrawAreaAndLink overwrites both r2/r3 with the
  // OT-tag residue. Mirror both branches exactly.
  const uint8_t flag = c->mem_r8(kHudGaugeBase + kFlagOff);
  c->r[2] = 1;
  c->r[3] = flag;
  if (flag == 1) {
    buildDrawAreaRect(c, kRectOff, kPanelX, (uint16_t)(hudViewportY(c) + kPanelYBias), kPanelW, kPanelH);
    emitDrawAreaAndLink(c, kRaFrameTileB, kRectOff);
  }
}

void HudGaugeEmitter::emitItem(Core* c) {
  // Frame: sp -= 64, spill s0..s6/ra at their RE'd offsets.
  static constexpr GuestFrameSpill kSpills[] = {
    {17, 36}, {31, 60}, {22, 56}, {21, 52}, {20, 48}, {19, 44}, {18, 40}, {16, 32},
  };
  GuestFrame<64, 8> frame(c, kSpills);
  constexpr uint32_t kWord0Off = 16; // sp+16..19 — record's word0, inset -4 (rect x / y-high)
  constexpr uint32_t kWord1Off = 20; // sp+20..23 — record's word1, inset +8 (rect w / h)
  constexpr uint32_t kRectOff  = 24; // sp+24..30 — {x,y,w,h} scratch for FUN_80081CF8's param_2

  GuestReg<17> rec(c); rec = c->r[4]; // record address, held live for the whole leaf
  const uint32_t sp = c->r[29];
  const HudGaugeItemRecord item{c, (uint32_t)rec};

  // Stage the record's rect-inset words into stack scratch (an unaligned lwl/lwr load in gen —
  // functionally an ordinary 32-bit read/write pair; mirrored plainly). x0/y-high keep their raw
  // value; w0/h keep theirs too, only the LOW halves (x, w) get the -4/+8 inset applied.
  c->mem_w32(sp + kWord0Off, c->mem_r32((uint32_t)rec + 0));
  c->mem_w32(sp + kWord1Off, c->mem_r32((uint32_t)rec + 4));
  c->mem_w16(sp + kWord0Off, (uint16_t)((int16_t)c->mem_r16(sp + kWord0Off) - 4));
  c->mem_w16(sp + kWord1Off, (uint16_t)((int16_t)c->mem_r16(sp + kWord1Off) + 8));

  const uint8_t kind = item.kind();
  // gen loads the packet-pool base into r20 (32780<<16) in the branch-delay slot reached for every
  // kind<3 record (gen_func_8004FB4C:6609) and keeps it live through the record's nested leaves;
  // FUN_8005019C spills it (sp+48). For kind>=3 gen branches out one instruction earlier and never
  // loads it, so r20 stays the incoming value. Mirror exactly (SBS core A MIRROR_VERIFY caught this
  // as native r20=0 vs substrate 0x800C0000 in FUN_8005019C's spill).
  if (kind < 3) c->r[20] = kPktPoolBaseReg;
  if (kind < 3 && kind != 0) {
    // This item also owns its own scissor box: the fixed full-viewport tile (identical to
    // emitFrame's tile A) plus a per-item "itemBox" tile derived from the inset rect staged
    // above. Both DR_AREA calls reuse sp+24 (matches gen's r21 = sp+24, held live across both).
    // gen keeps the rect-scratch pointer live in r21 across both DR_AREA builds and the final
    // FUN_8005019C (which spills it, sp+52). Mirror it (SBS core A MIRROR_VERIFY caught native
    // r21=incoming vs substrate sp+24).
    c->r[21] = sp + kRectOff;
    buildDrawAreaRect(c, kRectOff, kViewportX, hudViewportY(c), kViewportW, kViewportH);
    emitDrawAreaAndLink(c, kRaItemTileA, kRectOff);

    // gen loads r19 = OT-base register and r18 = OT tag here (right after tile A, before the first
    // FUN_8004EB94), keeps them live through the segment leaves + FUN_8005019C which all spill
    // them (r18@sp+40, r19@sp+44). The tile-A OT link above already used the same constants
    // inline; these assignments make the SPILLED register values byte-match gen.
    c->r[19] = kOtBaseReg;
    c->r[18] = kDrawAreaOtTag;
    emitSegmentLayout(c, kRaItemSeg1, (uint32_t)rec + kSegPrimaryOff, item, 0);
    if (kind == 1) {
      emitSegmentLayout(c, kRaItemSeg2, (uint32_t)rec + kSegSecondaryOff, item, -8);
    }

    const uint16_t itemX = c->mem_r16(sp + kWord0Off);
    const uint16_t itemYHigh = c->mem_r16(sp + kWord0Off + 2);
    const uint16_t itemW = c->mem_r16(sp + kWord1Off);
    const uint16_t itemH = c->mem_r16(sp + kWord1Off + 2);
    buildDrawAreaRect(c, kRectOff, itemX, (uint16_t)(itemYHigh + hudViewportY(c)), itemW, itemH);
    emitDrawAreaAndLink(c, kRaItemTileB, kRectOff);
  } else {
    emitSegmentLayout(c, kRaItemSegElse, (uint32_t)rec + kSegPrimaryOff, item, 0);
  }

  guest_fn(c, kFunLabelOrDigits, kRaItemFinal, sp + kWord0Off, item.labelByte(), 0u, 3u);
}

// ---- FUN_8004EB94 tap — the gauge's CENTERED 8x8-GLYPH TEXT ROW (RE 2026-07-16, gen_func_8004EB94
// shard_3.c:12762 + measure leaf gen_func_8004EA4C shard_1.c:8382) --------------------------------
// Despite the old kFunSegmentLayout name, this leaf draws a TEXT ROW: it walks a byte string at a0
// until 0xFF, emitting one op-0x75 SPRT_8x8 per glyph byte into the packet pool (each spliced
// individually into OT bucket 3), then a trailing DR_TPAGE(0x1F) header via func_80083DE0.
//   byte 0xF0..0xF7  -> palette-row select ((b+16)&0xFF = row 0..7), no emit, NO x advance
//   byte 0xFB        -> space: no emit, x += 8
//   other (until FF) -> glyph: uv = ((b&31)<<3, (b>>5)<<3), clut = ((row+496)<<6)|63, x += 8
// Start x = 160 - width/2, width from the measure leaf FUN_8004EA4C (8 per byte<192 or ==0xFB,
// zero for 0xC0..0xF9, terminates on 0xFA/0xFF). y = a1 (s16). Color bytes at pkt+4..+6 are left
// UNWRITTEN by the guest (op 0x75 = raw texture ignores them) — the tap writes no guest byte at all.
// The tap runs the gen body (guest state byte-exact) then re-derives the same glyph walk host-side
// and pushes RQ_HUD quads — same tap shape as game/ui/panel.cpp. This gives the gauge text/digits
// row its pc_render picture (the 9-slice box comes via the panelBuild tap).
namespace {
void gaugeTextRowTap(Core* c) {
  const uint32_t desc = c->r[4];
  const int y = (int32_t)(int16_t)(uint16_t)c->r[5];
  gen_func_8004EB94(c);
  if (c->game->oracle || c->rsub.mode.psxRender()) return;   // guest OT walk owns the picture
  if (c->mem_r8(desc) == 0xFFu) return;                          // empty row (gen early-exit)

  // Width per the measure leaf's rules (NOT the emit loop's — the guest centers on THIS number).
  int width = 0;
  for (uint32_t p = desc;; p++) {
    const uint8_t b = c->mem_r8(p);
    if (b == 0xFAu || b == 0xFFu) break;
    if (b < 192u || b == 0xFBu) width += 8;
  }
  int x = 160 - (width >> 1);

  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  unsigned paletteRow = 0;
  static long rows = 0;
  if ((rows++ & 63) == 0)
    cfg_logf("gaugeq", "text row desc=%08X y=%d width=%d first=%02X", desc, y, width, c->mem_r8(desc));
  for (uint32_t p = desc;; p++) {
    const uint8_t b = c->mem_r8(p);
    if (b == 0xFFu) break;
    if (((unsigned)(b + 16) & 0xFFu) < 8u) { paletteRow = (unsigned)(b + 16) & 0xFFu; continue; }
    if (b != 0xFBu) {
      const int u = (b & 31) << 3, v = (b >> 5) << 3;
      const uint32_t clut = ((paletteRow + 496u) << 6) | 63u;
      int xs[4] = { x + ox, x + 8 + ox, x + ox, x + 8 + ox };
      int ys[4] = { y + oy, y + oy, y + 8 + oy, y + 8 + oy };
      int us[4] = { u, u + 8, u, u + 8 };
      int vs[4] = { v, v, v + 8, v + 8 };
      unsigned char cc[4] = { 0x80, 0x80, 0x80, 0x80 };
      c->game->activeRq().push2dQuad(RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                     /*tp_x=*/960, /*tp_y=*/256, /*mode=*/0, /*raw=*/1,
                                     (int)(clut & 0x3F) * 16, (int)(clut >> 6) & 0x1FF,
                                     0, 0, 0, 0, 0, 0, 1023, 511);
    }
    x += 8;
  }
}
} // namespace

void HudGaugeEmitter::registerOverrides(Game*) {
  extern void gen_func_8004FD30(Core*);
  extern void gen_func_8004FB4C(Core*);
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8004FD30u, &HudGaugeEmitter::emitFrame, gen_func_8004FD30);
  engine_set_override_main(0x8004FB4Cu, &HudGaugeEmitter::emitItem,  gen_func_8004FB4C);
  engine_set_override_main(0x8004EB94u, gaugeTextRowTap,             gen_func_8004EB94);
}
