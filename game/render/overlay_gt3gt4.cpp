// game/render/overlay_gt3gt4.cpp — native mirror of the A00-overlay GT3/GT4 packet-emitter
// cluster. See overlay_gt3gt4.h for the wiring rationale (faithful substrate mirror, NOT
// pc_render); this file is the RE trace + the byte-exact transcription.
//
// RE: Ghidra headless decompile of a live seaside-field RAM dump (scratch/decomp/render146.c,
// FUN_80146478 / FUN_801465ec / FUN_801467bc) cross-checked against the recompiler's own
// register-accurate translation (generated/ov_a00_shard_0.c ov_a00_gen_80146478/801465EC,
// generated/ov_a00_shard_1.c ov_a00_gen_801467BC — the recompiler output is a strict per-
// instruction transcription so it is the more precise source for GTE register indices/opcodes,
// which Ghidra's COP2 decompilation garbles into placeholder immediates).
//
// FUN_80146478(rec_header, ot_base): uVar2 = *rec_header (low16 = GT3 count, high16 = GT4 count);
//   gt3(rec_header+16, ot_base, uVar2&0xffff) -> returns the GT4 array base; gt4(that, ot_base,
//   uVar2>>16). A thin 2-instruction dispatcher around the two leaves below — NOT separately
//   overridden (see overlay_gt3gt4.h): both leaves are wired directly so every call site (the
//   busy dispatcher AND a duplicate tail-shared copy of the same sequence the recompiler folded
//   into FUN_80147FC4) is covered uniformly.
//
// Both leaves emit the SAME PSX GP0 packet shapes the main engine's submit.cpp targets
// (POLY_GT3 / POLY_GT4 — gouraud-textured tri/quad), but via the GTE (RTPT + NCLIP + AVSZ3/a
// custom near-clamped Z blend) into the shared packet-pool bump allocator + OT linked list —
// this is the render-UNDERNEATH execution (psx_render's own subsystem), not the pc_render
// display pass. Every GTE op / guest write below is REQUIRED, not incidental: SBS gates it on
// both cores.
#include "core.h"
#include "game.h"
#include "overlay_gt3gt4.h"
#include "cfg.h"
#include <stdio.h>

#define PKT_POOL_PTR  0x800BF544u   // packet-pool bump-allocator write pointer (see pkt_span.h;
                                    // the pool span itself is [0x800BFE68, 0x800E7E68))
#define COL_MASK      0xFFF0F0F0u  // low-nibble-per-byte clear on RGB889 words (matches the GPU;
                                   // same constant as game/render/submit.cpp)

// Shared near-plane-aware OTZ pick: given the record's "blend/flag" byte (top byte of the record's
// second colour word) and the 3 raw SZ FIFO values (GTE data regs 17/18/19, i.e. SZ1/SZ2/SZ3 —
// RTPT/RTPS write SZ1..SZ3, leaving SZ0 the prior value), reproduce the exact clamp-then-average
// the recomp body performs when flag != 0. flag&2 selects min-clamp (additive-style blend, whose
// nearer faces should NOT lose the depth sort to a farther match) vs max-clamp (subtractive-style).
static int32_t overlay_gt_z_blend(uint32_t flag, int32_t sz1, int32_t sz2, int32_t sz3) {
  int32_t a = sz1, b = sz2;
  if (flag & 2u) {
    if (a - b >= 0) a = b;                 // additive: clamp a down to the nearer of (a,b)
    int32_t r = a >> 2;
    if (a - sz3 >= 0) r = sz3 >> 2;         // then to the nearer of (that, sz3)
    return r;
  } else {
    if (a - b <= 0) a = b;                 // subtractive: clamp a up to the farther of (a,b)
    int32_t r = a >> 2;
    if (a - sz3 <= 0) r = sz3 >> 2;         // then to the farther of (that, sz3)
    return r;
  }
}

// Shared OT-index compute from a raw OTZ-domain value (same bit-recombination + range gate in
// both leaves): pull the exponent nibble (bits [12:10] treated as a shift count), rebuild an
// index, then gate to the live OT range [4, 0x7ff) before it's used to index the bucket array.
// Returns -1 if the record is out of the OT's representable depth range (record dropped, exactly
// as the recomp body silently drops it — packet-pool writes already made are simply orphaned,
// the same "written but pointer never advanced" pattern pkt_span.h documents for other unowned
// renderers).
static int32_t overlay_gt_otz_index(int32_t z) {
  int32_t shift = z >> 10;
  int32_t idx = (z >> (shift & 31)) + shift * 0x200;
  if (!(idx - 0x7ff < 0)) return -1;
  if (!(idx - 4 > 0)) return -1;
  return idx;
}

// FUN_801465EC — POLY_GT3 (gouraud-textured triangle) emit, GTE-driven, guest-writing.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1(rgb2=rgb1<<4)|flag@[31:24], +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2hi(hi)}.
// Output POLY_GT3 packet = 40 bytes (10 words): {+0 OT tag(len=9<<24|next), +4 rgb0|code RAW
//   (unmasked — a real asymmetry vs the GT4 leaf below and vs submit.cpp's own GT3, verified by
//   the recomp body: this record's colour0 word never passes through COL_MASK), +8 SXY0,
//   +12 uv0|clut, +16 rgb1&MASK, +20 SXY1, +24 uv1|tpage, +28 rgb2&MASK, +32 SXY2, +36 uv2hi}.
void OverlayGt3Gt4::gt3(Core* c) {
  uint32_t rec = c->r[4], ot_base = c->r[5], count = c->r[6];
  if (cfg_dbg("ovgt")) { static long n=0; if (n++%512==0) cfg_logf("ovgt", "gt3 call#%ld count=%u", n, count); }
  if (count == 0) { c->r[2] = rec; return; }
  uint32_t pool = c->mem_r32(PKT_POOL_PTR);
  for (; count != 0; count--, rec += 36) {
    gte_write_data(0, c->mem_r32(rec + 16));              // VXY0
    uint32_t vz01 = c->mem_r32(rec + 20);
    gte_write_data(2, c->mem_r32(rec + 24));              // VXY1
    gte_write_data(1, vz01);                              // VZ0
    gte_write_data(4, c->mem_r32(rec + 28));              // VXY2
    uint32_t vz23 = c->mem_r32(rec + 32);
    gte_write_data(3, vz01 >> 16);                         // VZ1
    gte_write_data(5, vz23);                               // VZ2
    c->mem_w32(pool + 36, vz23 >> 16);                     // uv2hi staged (pre-RTPT scratch write)
    gte_op(c, 0x4A280030u);                                // RTPT (triple perspective transform)

    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12);
    uint32_t rgb0_code = c->mem_r32(rec + 0), rgb1_src = c->mem_r32(rec + 4);
    uint32_t flagreg = gte_read_ctrl(31);
    c->mem_w32(pool + 12, uv0);
    c->mem_w32(pool + 24, uv1);                            // both writes happen regardless of flag
    if ((int32_t)flagreg < 0) continue;                    // GTE FLAG error -> drop this record

    c->mem_w32(pool + 8,  gte_read_data(12));               // SXY0
    c->mem_w32(pool + 20, gte_read_data(13));               // SXY1
    c->mem_w32(pool + 32, gte_read_data(14));               // SXY2
    int32_t sxy0 = (int32_t)gte_read_data(12), sxy1 = (int32_t)gte_read_data(13), sxy2 = (int32_t)gte_read_data(14);

    // frustum reject: unsigned-compare the packed SXY words against 240<<16, then (after <<16
    // each) against 320<<16 — the recomp body's own screen-bound test, reproduced literally.
    uint32_t t240 = 240u << 16;
    bool any1 = ((uint32_t)sxy0 < t240) || ((uint32_t)sxy1 < t240) || ((uint32_t)sxy2 < t240);
    if (!any1) continue;
    uint32_t t320 = 320u << 16;
    uint32_t s0s = (uint32_t)sxy0 << 16, s1s = (uint32_t)sxy1 << 16, s2s = (uint32_t)sxy2 << 16;
    bool any2 = (s0s < t320) || (s1s < t320) || (s2s < t320);
    if (!any2) continue;

    gte_op(c, 0x4B400006u);                                 // NCLIP (backface / MAC0)
    c->mem_w32(pool + 16, rgb1_src & COL_MASK);              // rgb1
    c->mem_w32(pool + 4,  rgb0_code);                        // rgb0|code, UNMASKED (faithful)
    uint32_t rgb2 = (rgb1_src << 4) & COL_MASK;
    int32_t mac0 = (int32_t)gte_read_data(24);
    c->mem_w32(pool + 28, rgb2);
    if (mac0 <= 0) continue;                                 // backface cull

    uint32_t flagbyte = rgb1_src >> 24;
    int32_t z;
    if (flagbyte == 0) {
      gte_op(c, 0x4B58002Du);                                // AVSZ3 (straight OTZ average)
      z = (int32_t)gte_read_data(7);
    } else {
      int32_t sz1 = (int32_t)gte_read_data(17), sz2 = (int32_t)gte_read_data(18), sz3 = (int32_t)gte_read_data(19);
      z = overlay_gt_z_blend(flagbyte, sz1, sz2, sz3);
    }

    int32_t idx = overlay_gt_otz_index(z);
    if (idx < 0) continue;

    uint32_t* slot = nullptr; (void)slot;
    uint32_t slot_addr = ot_base + (uint32_t)idx * 4;
    uint32_t old_head = c->mem_r32(slot_addr);
    c->mem_w32(slot_addr, pool);                             // OT bucket head = new packet
    c->mem_w32(pool + 0, old_head | (9u << 24));              // tag: len=9 data words | old head
    pool += 40;
  }
  c->mem_w32(PKT_POOL_PTR, pool);
  c->r[2] = rec;
}

// FUN_801467BC — POLY_GT4 (gouraud-textured quad) emit, GTE-driven, guest-writing.
// Record = 44 bytes: {+0 rgb0|code, +4 rgb2(rgb3=rgb2<<4)|flag@[31:24], +8 uv0|clut, +12 uv1|tpage,
//   +16 uv2(lo)|uv3(hi), +20 VXY0, +24 VZ0(lo)|VZ1(hi), +28 VXY1, +32 VXY2, +36 VZ2(lo)|VZ3(hi),
//   +40 VXY3}. VXY3/VZ3 are transformed by a separate RTPS (the GTE's RTPT only handles 3 points).
// Output POLY_GT4 packet = 52 bytes (13 words): {+0 tag(len=0xC<<24|next), +4 rgb0&MASK, +8 SXY0,
//   +12 uv0|clut, +16 rgb1&MASK, +20 SXY1, +24 uv1|tpage, +28 rgb2&MASK, +32 SXY2, +36 uv2,
//   +40 rgb3&MASK, +44 SXY3, +48 uv3}. Unlike the GT3 leaf above, rgb0 here IS masked — verified
// against the recomp body, not "fixed" to match GT3 (the asymmetry is faithful, not a bug).
void OverlayGt3Gt4::gt4(Core* c) {
  uint32_t rec = c->r[4], ot_base = c->r[5], count = c->r[6];
  if (count == 0) { c->r[2] = rec; return; }
  uint32_t pool = c->mem_r32(PKT_POOL_PTR);
  for (; count != 0; count--, rec += 44) {
    gte_write_data(0, c->mem_r32(rec + 20));               // VXY0
    uint32_t vz01 = c->mem_r32(rec + 24);
    gte_write_data(2, c->mem_r32(rec + 28));               // VXY1
    gte_write_data(1, vz01);                                // VZ0
    gte_write_data(4, c->mem_r32(rec + 32));               // VXY2
    uint32_t vz23 = c->mem_r32(rec + 36);
    gte_write_data(3, vz01 >> 16);                          // VZ1
    gte_write_data(5, vz23);                                // VZ2
    gte_op(c, 0x4A280030u);                                 // RTPT (verts 0..2)

    uint32_t flagreg = gte_read_ctrl(31);
    if ((int32_t)flagreg < 0) continue;
    gte_op(c, 0x4B400006u);                                  // NCLIP (backface / MAC0)
    int32_t mac0 = (int32_t)gte_read_data(24);
    if (mac0 <= 0) continue;                                 // backface cull

    c->mem_w32(pool + 8,  gte_read_data(12));                // SXY0
    c->mem_w32(pool + 20, gte_read_data(13));                // SXY1
    c->mem_w32(pool + 32, gte_read_data(14));                // SXY2

    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12);
    uint32_t rgb0_code = c->mem_r32(rec + 0), rgb2_src = c->mem_r32(rec + 4);
    uint32_t uv23 = c->mem_r32(rec + 16);
    c->mem_w32(pool + 12, uv0);
    c->mem_w32(pool + 24, uv1);
    c->mem_w32(pool + 4,  rgb0_code & COL_MASK);             // rgb0, MASKED (differs from GT3 leaf)
    c->mem_w32(pool + 16, (rgb0_code << 4) & COL_MASK);      // rgb1
    c->mem_w32(pool + 28, rgb2_src & COL_MASK);              // rgb2
    c->mem_w32(pool + 40, (rgb2_src << 4) & COL_MASK);       // rgb3
    c->mem_w32(pool + 36, uv23);                             // uv2 (lo half)
    c->mem_w32(pool + 48, uv23 >> 16);                       // uv3 (hi half)

    gte_write_data(0, c->mem_r32(rec + 40));                 // VXY3
    gte_write_data(1, vz23 >> 16);                            // VZ3
    gte_op(c, 0x4A180001u);                                  // RTPS (4th point, single transform)
    uint32_t flagreg2 = gte_read_ctrl(31);
    if ((int32_t)flagreg2 < 0) continue;
    c->mem_w32(pool + 44, gte_read_data(14));                // SXY3

    uint32_t flagbyte = rgb2_src >> 24;
    int32_t z;
    if (flagbyte == 0) {
      gte_op(c, 0x4B68002Eu);                                 // AVSZ4 (straight OTZ average, 4 pts)
      z = (int32_t)gte_read_data(7);
    } else {
      int32_t sz1 = (int32_t)gte_read_data(16), sz2 = (int32_t)gte_read_data(17),
              sz3 = (int32_t)gte_read_data(18), sz4 = (int32_t)gte_read_data(19);
      // 4-point variant: clamp sz1 vs sz2 and sz3 vs sz4 pairwise first, then combine — the
      // recomp body's exact widened form of the 3-point blend above (see FUN_801467bc decomp).
      int32_t a = sz1, b = sz2, e = sz3, f = sz4;
      if (flagbyte & 2u) {
        if (a - b >= 0) a = b;
        if (e - f >= 0) e = f;
        int32_t r = a >> 2;
        if (a - e >= 0) r = e >> 2;
        z = r;
      } else {
        if (a - b <= 0) a = b;
        if (e - f <= 0) e = f;
        int32_t r = a >> 2;
        if (a - e <= 0) r = e >> 2;
        z = r;
      }
    }

    int32_t idx = overlay_gt_otz_index(z);
    if (idx < 0) continue;

    uint32_t slot_addr = ot_base + (uint32_t)idx * 4;
    uint32_t old_head = c->mem_r32(slot_addr);
    c->mem_w32(slot_addr, pool);
    c->mem_w32(pool + 0, old_head | (0xCu << 24));           // tag: len=12 data words | old head
    pool += 52;
  }
  c->mem_w32(PKT_POOL_PTR, pool);
  c->r[2] = rec;
}

void OverlayGt3Gt4::registerOverrides(Game*) {
  // engine_set_override_a00 (runtime/recomp/override_registry.h) installs into the ONE process-global
  // override registry, which runs ov_a00_gen_* on the oracle leg (core B) and the native handler
  // everywhere else — NOT a raw ov_a00_set_override, since these are engine/game natives and the
  // oracle must run the pure recompiled body.
  extern void ov_a00_gen_801465EC(Core*);
  extern void ov_a00_gen_801467BC(Core*);
  extern void engine_set_override_a00(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_a00(0x801465ECu, &OverlayGt3Gt4::gt3, ov_a00_gen_801465EC);
  engine_set_override_a00(0x801467BCu, &OverlayGt3Gt4::gt4, ov_a00_gen_801467BC);
}
