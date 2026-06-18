// Native ownership of the engine's geometry SUBMIT path (Tomba2Engine).
//
// These are the resident routines that take an object's pre-built primitive-record list, GTE-project
// each record's model vertices, backface/frustum-cull, compute an ordering-table (OT) bucket, write the
// screen-space GPU packet, and link it into the OT. The recompiled MIPS bodies threw away the float
// view-space depth (only integer SXY survives into the packet), which is the ONLY reason the value-keyed
// "attach" measurement-hack existed (recovering depth by correlating projected SXY against memory
// stores). By owning the submit code natively we compute the projection and KEEP the real per-vertex
// view-Z, carrying it straight to the renderer's depth path — no correlation, no bridge.
//
// Faithful-first: the native routine reproduces the recomp body BYTE-FOR-BYTE (identical packets, OT
// links, packet-pool advance, cull decisions, return value), verified 0-diff vs the recomp body on real
// field gameplay. PSXPORT_SUBMIT_RECOMP=1 keeps the recomp bodies for A/B. The GTE math itself stays a
// platform primitive (gte_op → the Beetle GTE), exactly as the recomp body called it, so projection
// results are bit-identical; we own the control flow, record decode, packet assembly and OT insertion.
//
// RE (recomp bodies gen_func_8007FDB0 / gen_func_8008007C, decoded into clean form — docs/engine_re.md):
//   args: a0 = primitive-record array, a1 = OT base, a2 = record count;  returns a0 advanced past the array.
//   global packet-pool write pointer at 0x800BF544 (advanced past each committed packet).
#include "r3000.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>

void rec_super_call(R3000*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

#define PKT_POOL_PTR 0x800BF544u   // DAT_800bf544: current free GPU-packet write pointer
#define COL_MASK     0xFFF0F0F0u   // low-nibble-per-byte clear applied to packet RGB words

static int s_submit_recomp = -1;   // PSXPORT_SUBMIT_RECOMP=1 -> keep the recomp body (A/B)
static int submit_recomp(void) { if (s_submit_recomp < 0) s_submit_recomp = cfg_on("PSXPORT_SUBMIT_RECOMP") ? 1 : 0;
                                 return s_submit_recomp; }

// PC-native per-vertex depth (Phase 2): because we OWN the projection, we know each vertex's real
// view-space Z (the SZ the GTE just produced) — record it keyed by the packet vertex word's address so
// the renderer's D32 depth buffer does true per-pixel occlusion (PSXPORT_NATIVE_DEPTH / the SBS A/B
// view) instead of OT-submission order. No correlation, no value-matching: the engine that emits the
// vertex writes the depth for the exact address it stored the SXY to. Off (faithful) by default.
int  attach_enabled(void);                       // native-depth path live (gte_beetle.c)
void projprim_set_pz(uint32_t addr, float pz);   // record a vertex's view-Z at its packet word address
void proj_set_H(uint16_t h);                     // tell proj_pz_to_ord the projection-plane H (CR26)
static int s_depth = -1;
static int depth_on(void) { if (s_depth < 0) s_depth = attach_enabled() ? 1 : 0; return s_depth; }

// GTE opcodes used by the submit path (cop2 instruction encodings, matching the recomp bodies).
#define GTE_RTPS  0x4A180001u   // project 1 vertex (V0)            -> SXY2/SZ3
#define GTE_RTPT  0x4A280030u   // project 3 vertices (V0,V1,V2)    -> SXY0/1/2, SZ1/2/3
#define GTE_NCLIP 0x4B400006u   // signed area of (SXY0,SXY1,SXY2)  -> MAC0 (>0 front-facing)
#define GTE_AVSZ3 0x4B58002Du   // average Z of the 3 SZ           -> OTZ (DR7), scaled by ZSF3
#define GTE_AVSZ4 0x4B68002Eu   // average Z of the 4 SZ           -> OTZ (DR7), scaled by ZSF4

// OT-bucket depth from the SZ FIFO. `nz` SZ regs starting at DR `zbase` (tri: 3 @ DR17; quad: 4 @ DR16).
// The record's code byte selects: type 1 -> farthest (max SZ)>>2, type 2 -> nearest (min SZ)>>2,
// else -> hardware AVSZ average. (Verified by exhaustively tracing each recomp body's branch tree —
// both type paths reduce to a pure min/max over all the SZ.)
static uint32_t ot_depth(R3000* c, uint32_t code, int zbase, int nz, uint32_t avsz) {
  uint32_t type = (code >> 24) & 3u;
  if (type == 1 || type == 2) {
    int32_t z = (int32_t)gte_read_data(zbase);
    for (int i = 1; i < nz; i++) { int32_t zi = (int32_t)gte_read_data(zbase + i);
      if (type == 1 ? (zi > z) : (zi < z)) z = zi; }
    return (uint32_t)(z >> 2);
  }
  gte_op(c, avsz);
  return gte_read_data(7);
}

// Logarithmic OT-bucket compression + range clamp, exactly as the recomp body. Returns the final OT
// index, or 0xFFFFFFFF (negative) if out of the drawable range [4,2047] (prim culled, not linked).
// Uses ARITHMETIC shifts to match the recomp bodies bit-for-bit when otz can be negative (the
// byte-packed variants add a signed per-call OT-Z bias before this; for the non-negative AVSZ inputs
// of the resident GT3/GT4 library this is identical to a logical shift).
static uint32_t ot_compress(uint32_t otz) {
  int32_t z = (int32_t)otz;
  int32_t sh = z >> 10;
  int32_t idx = (z >> (sh & 31)) + (sh << 9);
  if ((uint32_t)(idx - 4) < 2044u) return (uint32_t)idx;   // in range
  return 0xFFFFFFFFu;                                       // out of range -> -1 -> skip
}

// gen_func_8007FDB0 — POLY_GT3 (gouraud-textured triangle) submit.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1 (rgb2 = rgb1<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2(hi)}.
// Packet = 40 bytes POLY_GT3: {tag, rgb0|code, SXY0, uv0|clut, rgb1, SXY1, uv1|tpage, rgb2, SXY2, uv2}.
static void submit_poly_gt3(R3000* c) {
  uint32_t rec = c->r[4], ot = c->r[5], count = c->r[6];
  uint32_t pkt = mem_r32(PKT_POOL_PTR);
  int depth = depth_on(); if (depth) proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 36) {
    // load the 3 model vertices into the GTE input regs (V0..V2), then project all three (RTPT).
    uint32_t vz01 = mem_r32(rec + 20);
    gte_write_data(0, mem_r32(rec + 16));          // VXY0
    gte_write_data(1, vz01 & 0xFFFFu);             // VZ0
    gte_write_data(2, mem_r32(rec + 24));          // VXY1
    gte_write_data(3, vz01 >> 16);                 // VZ1
    gte_write_data(4, mem_r32(rec + 28));          // VXY2
    gte_write_data(5, mem_r32(rec + 32));          // VZ2 (low 16)
    uint32_t code = mem_r32(rec + 4);
    mem_w32(pkt + 4,  mem_r32(rec + 0));           // rgb0|code
    gte_op(c, GTE_RTPT);
    mem_w32(pkt + 12, mem_r32(rec + 8));           // uv0|clut
    mem_w32(pkt + 24, mem_r32(rec + 12));          // uv1|tpage
    if ((int32_t)gte_read_ctrl(31) < 0) continue;  // GTE FLAG: projection error/overflow -> drop
    gte_op(c, GTE_NCLIP);
    mem_w32(pkt + 16, code & COL_MASK);            // rgb1
    if ((int32_t)gte_read_data(24) <= 0) continue; // MAC0 = signed area <= 0 -> backface -> drop
    mem_w32(pkt + 8,  gte_read_data(12));          // SXY0
    mem_w32(pkt + 20, gte_read_data(13));          // SXY1
    mem_w32(pkt + 32, gte_read_data(14));          // SXY2
    // frustum cull (right/bottom edges only, as the original): drop if all 3 SX>=320 or all 3 SY>=240.
    uint16_t sx0 = mem_r16(pkt + 8),  sx1 = mem_r16(pkt + 20), sx2 = mem_r16(pkt + 32);
    if (sx0 >= 320 && sx1 >= 320 && sx2 >= 320) continue;
    uint16_t sy0 = mem_r16(pkt + 10), sy1 = mem_r16(pkt + 22), sy2 = mem_r16(pkt + 34);
    if (sy0 >= 240 && sy1 >= 240 && sy2 >= 240) continue;
    mem_w32(pkt + 28, (code << 4) & COL_MASK);     // rgb2
    uint32_t idx = ot_compress(ot_depth(c, code, 17, 3, GTE_AVSZ3));
    if ((int32_t)idx < 0) continue;                // out of OT range -> drop
    mem_w16(pkt + 36, mem_r16(rec + 34));          // uv2 (high half of rec+32 word)
    uint32_t otaddr = ot + (idx << 2);
    mem_w32(pkt + 0, mem_r32(otaddr) | 0x09000000u);  // tag: link old head + length (9 words)
    mem_w32(otaddr, pkt);                          // OT head -> this packet
    if (depth) {                                   // record each vertex's real view-Z at its packet addr
      projprim_set_pz(pkt + 8,  (float)(int32_t)gte_read_data(17));   // SXY0 -> SZ0
      projprim_set_pz(pkt + 20, (float)(int32_t)gte_read_data(18));   // SXY1 -> SZ1
      projprim_set_pz(pkt + 32, (float)(int32_t)gte_read_data(19));   // SXY2 -> SZ2
    }
    pkt += 40;
  }
  mem_w32(PKT_POOL_PTR, pkt);
  c->r[2] = rec;                                   // return: record pointer advanced past the array
}

void ov_submit_poly_gt3(R3000* c) {
  if (submit_recomp()) { rec_super_call(c, 0x8007FDB0u); return; }
  submit_poly_gt3(c);
}

// gen_func_8008007C — POLY_GT4 (gouraud-textured quad) submit.
// Record = 44 bytes: {+0 rgb0(rgb1=<<4), +4 rgb2(rgb3=<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 uv2(lo)|uv3(hi), +20 VXY0, +24 VZ0(lo)|VZ1(hi), +28 VXY1, +32 VXY2, +36 VZ2(lo)|VZ3(hi), +40 VXY3}.
// Packet = 52 bytes POLY_GT4: {tag, rgb0,SXY0,uv0|clut, rgb1,SXY1,uv1|tpage, rgb2,SXY2,uv2, rgb3,SXY3,uv3}.
// The 4th vertex (V3) is projected alone via RTPS first, then the front tri (V0,V1,V2) via RTPT.
static void submit_poly_gt4(R3000* c) {
  uint32_t rec = c->r[4], ot = c->r[5], count = c->r[6];
  uint32_t pkt = mem_r32(PKT_POOL_PTR);
  int depth = depth_on(); if (depth) proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 44) {
    // project the lone 4th vertex (V3) first (RTPS): result SXY in DR14.
    uint32_t code2 = mem_r32(rec + 4);
    gte_write_data(0, mem_r32(rec + 40));          // VXY3
    gte_write_data(1, mem_r32(rec + 36) >> 16);    // VZ3
    mem_w32(pkt + 28, code2 & COL_MASK);           // rgb2
    gte_op(c, GTE_RTPS);
    mem_w32(pkt + 40, (code2 << 4) & COL_MASK);    // rgb3
    mem_w32(pkt + 24, mem_r32(rec + 12));          // uv1|tpage
    if ((int32_t)gte_read_ctrl(31) < 0) continue;  // GTE FLAG: V3 projection error -> drop
    mem_w32(pkt + 44, gte_read_data(14));          // SXY3
    // project the front triangle (V0,V1,V2) via RTPT.
    uint32_t vz01 = mem_r32(rec + 24);
    gte_write_data(0, mem_r32(rec + 20));          // VXY0
    gte_write_data(1, vz01 & 0xFFFFu);             // VZ0
    gte_write_data(3, vz01 >> 16);                 // VZ1
    gte_write_data(2, mem_r32(rec + 28));          // VXY1
    gte_write_data(4, mem_r32(rec + 32));          // VXY2
    gte_write_data(5, mem_r32(rec + 36) & 0xFFFFu);// VZ2
    uint32_t code0 = mem_r32(rec + 0);
    mem_w32(pkt + 4, code0 & COL_MASK);            // rgb0
    gte_op(c, GTE_RTPT);
    mem_w32(pkt + 16, (code0 << 4) & COL_MASK);    // rgb1
    if ((int32_t)gte_read_ctrl(31) < 0) continue;  // GTE FLAG -> drop
    gte_op(c, GTE_NCLIP);
    mem_w32(pkt + 12, mem_r32(rec + 8));           // uv0|clut
    if ((int32_t)gte_read_data(24) <= 0) continue; // backface (front-tri signed area <= 0) -> drop
    mem_w32(pkt + 8,  gte_read_data(12));          // SXY0
    mem_w32(pkt + 20, gte_read_data(13));          // SXY1
    mem_w32(pkt + 32, gte_read_data(14));          // SXY2
    // frustum cull (right/bottom edges) over all 4 verts.
    uint16_t sx0 = mem_r16(pkt + 8), sx1 = mem_r16(pkt + 20), sx2 = mem_r16(pkt + 32), sx3 = mem_r16(pkt + 44);
    if (sx0 >= 320 && sx1 >= 320 && sx2 >= 320 && sx3 >= 320) continue;
    uint16_t sy0 = mem_r16(pkt + 10), sy1 = mem_r16(pkt + 22), sy2 = mem_r16(pkt + 34), sy3 = mem_r16(pkt + 46);
    if (sy0 >= 240 && sy1 >= 240 && sy2 >= 240 && sy3 >= 240) continue;
    uint32_t idx = ot_compress(ot_depth(c, code2, 16, 4, GTE_AVSZ4));  // 4 SZ in DR16..19
    if ((int32_t)idx < 0) continue;                // out of OT range -> drop
    uint32_t uv23 = mem_r32(rec + 16);
    mem_w32(pkt + 36, uv23);                        // uv2 (low half used by GPU)
    mem_w32(pkt + 48, uv23 >> 16);                  // uv3
    uint32_t otaddr = ot + (idx << 2);
    mem_w32(pkt + 0, mem_r32(otaddr) | 0x0C000000u);  // tag: link old head + length (12 words)
    mem_w32(otaddr, pkt);                          // OT head -> this packet
    if (depth) {                                   // SZ FIFO after RTPS(V3)+RTPT(V0,V1,V2): DR16=SZ3,17-19=SZ0-2
      projprim_set_pz(pkt + 8,  (float)(int32_t)gte_read_data(17));   // SXY0 -> SZ0
      projprim_set_pz(pkt + 20, (float)(int32_t)gte_read_data(18));   // SXY1 -> SZ1
      projprim_set_pz(pkt + 32, (float)(int32_t)gte_read_data(19));   // SXY2 -> SZ2
      projprim_set_pz(pkt + 44, (float)(int32_t)gte_read_data(16));   // SXY3 -> SZ3
    }
    pkt += 52;
  }
  mem_w32(PKT_POOL_PTR, pkt);
  c->r[2] = rec;                                   // return: record pointer advanced past the array
}

void ov_submit_poly_gt4(R3000* c) {
  if (submit_recomp()) { rec_super_call(c, 0x8008007Cu); return; }
  submit_poly_gt4(c);
}

// =====================================================================================================
// Byte-packed POLY_GT4 submit variant — gen_func_80027768 (resident MAIN). A DISTINCT submitter from
// the GT3/GT4 library above: it is the field's dominant world-poly emitter (~252 GT4/frame) and ran
// interpreted, so it carried no native depth. Decoded from the recomp body (docs/engine_re.md):
//   ABI:  a0 = record array; a1 = CLUT-Y bank offset (added <<22 to the uv0|clut word);
//         a2 = OT-Z bias (sign-extended s16, added to AVSZ4 OTZ before the log-compress);
//         a3 = U-texture offset (added to the U byte of all four uv words).
//   NO count arg: the loop runs record-by-record and continues while the record's CONTROL word
//   (rec+4) is > 0 (its sign marks the last record, which is still drawn).
//   OT base is a GLOBAL (*0x800ED8C8), NOT an arg. IR0 depth-cue factor is read from scratchpad
//   0x1F800090 each iteration. Returns r2 = 0x800C0000 (the body leaves the pool-base reg there).
// Record (36 bytes): vertex X/Y/Z are SIGNED bytes scaled <<8 (a u16 GTE coord); the top byte of each
//   RGB word doubles as that vertex's Z, so X/Y live in rec[0x1C..0x23] and Z in the RGB words:
//     rec+0x00 uv0|clut(word) ->pkt+12   rec+0x04 control: low23 -> pkt+24 (uv1|clut), bit30 = semi-trans,
//     value>0 = more records follow      rec+0x08 uv2(lo)|uv3(hi) -> pkt+36/+48
//     rec+0x0C rgb0(+VZ0 in top byte) rec+0x10 rgb1(+VZ1) rec+0x14 rgb2(+VZ2) rec+0x18 rgb3(+VZ3)
//     X: rec+0x1C=VX0 0x1D=VX1 0x20=VX2 0x21=VX3   Y: 0x1E=VY0 0x1F=VY1 0x22=VY2 0x23=VY3
//     Z (top byte of the rgb word): 0x0F=VZ0 0x13=VZ1 0x17=VZ2 0x1B=VZ3
//   Colors: DPCT depth-cues rgb0/rgb1/rgb2, DPCS depth-cues rgb3 (both toward FAR_COLOR via IR0).
// Faithful-first: this reproduces the recomp body's writes/cull/return exactly (0-diff gate); when the
// native-depth path is live it additionally records each vertex's real view-Z (the SZ FIFO) keyed by
// its packet SXY-word address, exactly like the GT3/GT4 library above.
#define OTBASE_PTR   0x800ED8C8u             // *this = the active ordering-table base for these variants
#define IR0_SCRATCH  0x1F800090u             // depth-cue interpolation factor (IR0) staged in scratchpad
#define GTE_DPCT     0x4AF8002Au             // depth-cue 3 colors (rgb0,rgb1,rgb2) toward FAR_COLOR
#define GTE_DPCS     0x4A780010u             // depth-cue 1 color  (rgb)            toward FAR_COLOR

// byte b at `addr`, scaled <<8 into a 16-bit GTE coordinate (signedness is irrelevant after the 16-bit
// truncation: (b<<8)&0xFFFF == (sext8(b)<<8)&0xFFFF for any byte b — the recomp body sext's some lanes
// and not others, all yielding the same halfword).
static inline uint32_t vcoord(uint32_t addr) { return ((uint32_t)mem_r8(addr) << 8) & 0xFFFFu; }
// add the per-call U offset (a3) to the low (U) byte of a packet uv word, in place (mod 256).
static inline void uoff_add(uint32_t pkt_uv, uint32_t uoff) {
  mem_w8(pkt_uv, (uint8_t)(mem_r8(pkt_uv) + uoff));
}

static void submit_poly_gt4_bp(R3000* c) {
  uint32_t rec = c->r[4];
  uint32_t xoff = c->r[5] << 22;                       // CLUT-Y bank offset
  int32_t  zoff = (int32_t)(int16_t)c->r[6];           // OT-Z bias (sign-extended)
  uint32_t uoff = c->r[7];                             // U-texture offset
  uint32_t pkt  = mem_r32(PKT_POOL_PTR);
  uint32_t otbase = mem_r32(OTBASE_PTR);
  int depth = depth_on(); if (depth) proj_set_H((uint16_t)gte_read_ctrl(26));
  for (;;) {
    uint32_t ctl = mem_r32(rec + 4);                   // control word (sign = last record)
    // front triangle (V0,V1,V2) -> RTPT
    gte_write_data(0, vcoord(rec + 0x1C) | (vcoord(rec + 0x1E) << 16));  // VXY0
    gte_write_data(1, vcoord(rec + 0x0F));                                // VZ0
    gte_write_data(2, vcoord(rec + 0x1D) | (vcoord(rec + 0x1F) << 16));  // VXY1
    gte_write_data(3, vcoord(rec + 0x13));                                // VZ1
    gte_write_data(4, vcoord(rec + 0x20) | (vcoord(rec + 0x22) << 16));  // VXY2
    gte_write_data(5, vcoord(rec + 0x17));                                // VZ2
    gte_op(c, GTE_RTPT);
    if ((int32_t)gte_read_ctrl(31) >= 0) {
      mem_w32(pkt + 8,  gte_read_data(12));            // SXY0
      mem_w32(pkt + 20, gte_read_data(13));            // SXY1
      mem_w32(pkt + 32, gte_read_data(14));            // SXY2
      // 4th vertex (V3) -> RTPS
      gte_write_data(0, vcoord(rec + 0x21) | (vcoord(rec + 0x23) << 16)); // VXY3
      gte_write_data(1, vcoord(rec + 0x1B));                               // VZ3
      mem_w32(pkt + 12, mem_r32(rec + 0) + xoff);      // uv0|clut + CLUT bank
      gte_op(c, GTE_RTPS);
      if ((int32_t)gte_read_ctrl(31) >= 0) {
        mem_w32(pkt + 44, gte_read_data(14));          // SXY3
        mem_w32(pkt + 24, ctl & 0x7FFFFFu);            // uv1|clut (control low 23 bits)
        gte_op(c, GTE_AVSZ4);
        uint32_t idx = ot_compress((uint32_t)((int32_t)gte_read_data(7) + zoff));
        if ((int32_t)idx >= 0) {
          uint32_t uv = mem_r32(rec + 8);
          mem_w32(pkt + 36, uv);                       // uv2
          mem_w32(pkt + 48, (uint32_t)((int32_t)uv >> 16)); // uv3 (sign-extended high half)
          uoff_add(pkt + 12, uoff); uoff_add(pkt + 24, uoff);
          uoff_add(pkt + 36, uoff); uoff_add(pkt + 48, uoff);
          // depth-cued colors: DPCT(rgb0,rgb1,rgb2) then DPCS(rgb3)
          gte_write_data(8,  mem_r32(IR0_SCRATCH));    // IR0 (depth-cue factor)
          gte_write_data(20, mem_r32(rec + 0x0C));     // RGB0
          gte_write_data(21, mem_r32(rec + 0x10));     // RGB1
          gte_write_data(22, mem_r32(rec + 0x14));     // RGB2
          gte_write_data(6,  mem_r32(rec + 0x14));     // RGBC (= rgb2; written by the body)
          gte_op(c, GTE_DPCT);
          mem_w32(pkt + 4,  gte_read_data(20));        // rgb0 out
          mem_w32(pkt + 16, gte_read_data(21));        // rgb1 out
          mem_w32(pkt + 28, gte_read_data(22));        // rgb2 out
          mem_w8(pkt + 7, (ctl & 0x40000000u) ? 0x3E : 0x3C);  // code: semi-trans vs opaque GT4
          gte_write_data(6, mem_r32(rec + 0x18));      // RGB3 in
          gte_op(c, GTE_DPCS);
          mem_w32(pkt + 40, gte_read_data(22));        // rgb3 out
          uint32_t otaddr = otbase + (idx << 2);
          mem_w32(pkt + 0, mem_r32(otaddr) | 0x0C000000u);  // tag: link old head + len 12 (GT4)
          mem_w32(otaddr, pkt);
          if (depth) {                                  // SZ FIFO after RTPT(V0..2)+RTPS(V3): DR16..19 = SZ0..3
            projprim_set_pz(pkt + 8,  (float)(int32_t)gte_read_data(16));   // SXY0 -> SZ0
            projprim_set_pz(pkt + 20, (float)(int32_t)gte_read_data(17));   // SXY1 -> SZ1
            projprim_set_pz(pkt + 32, (float)(int32_t)gte_read_data(18));   // SXY2 -> SZ2
            projprim_set_pz(pkt + 44, (float)(int32_t)gte_read_data(19));   // SXY3 -> SZ3
          }
          pkt += 52;
        }
      }
    }
    if ((int32_t)ctl <= 0) break;                      // control sign marks the last record
    rec += 36;
  }
  mem_w32(PKT_POOL_PTR, pkt);
  c->r[2] = 0x800C0000u;                               // return value the recomp body leaves in r2
}

void ov_submit_poly_gt4_bp(R3000* c) {
  if (submit_recomp()) { rec_super_call(c, 0x80027768u); return; }
  submit_poly_gt4_bp(c);
}

// --- Auto-ownership of the SAME submit library when it appears in a runtime-loaded overlay --------
// The two resident submit fns above are also present, the same ALGORITHM (verified — only register
// allocation / scheduling / relocated internal calls differ), in per-area render overlays at
// addresses REUSED across scenes (e.g. the field's resident-region GAME overlay). They run
// interpreted and so don't carry native depth. Rather than hardcode per-scene addresses (fragile: a
// later scene can load different code there), the loader calls rec_overlay_loaded(base,size) after
// each overlay copy; we scan the freshly-loaded bytes ONCE for the submit library's signature and
// register the matching native impl at each entry (scan-on-load — no per-call cost). The override is
// cleared on the next overlay load, so an address is only ever owned while the real submit code is
// resident. Signature is POLY_GT3/GT4-specific (packet-pool load + RTPT + the OT tag-length
// immediate 9/12 words), so other primitive submitters (flat/sprite/line) won't match.
static OverrideFn classify_submit(uint32_t addr) {
  int has_pool = 0, has_rtpt = 0, gt3 = 0, gt4 = 0, prev_lui800c = 0;
  for (int i = 0; i < 320; i++) {            // entry .. first jr $ra (single epilogue in these fns)
    uint32_t w = mem_r32(addr + i * 4);
    if (w == 0x03E00008u) break;             // jr $ra -> function end
    if ((w >> 26) == 0x0Fu && (w & 0xFFFFu) == 0x800Cu) { prev_lui800c = 1; continue; }   // lui $r,0x800C
    if (prev_lui800c && (w >> 26) == 0x23u && (w & 0xFFFFu) == 0xF544u) has_pool = 1;      // lw $r,-0xABC(.) = &DAT_800bf544
    prev_lui800c = 0;
    if (w == 0x4A280030u) has_rtpt = 1;                                  // RTPT (project the front tri)
    if ((w >> 26) == 0x0Fu && (w & 0xFFFFu) == 0x0900u) gt3 = 1;         // lui $r,0x0900 -> POLY_GT3 tag len 9
    if ((w >> 26) == 0x0Fu && (w & 0xFFFFu) == 0x0C00u) gt4 = 1;         // lui $r,0x0C00 -> POLY_GT4 tag len 12
  }
  if (!has_pool || !has_rtpt) return 0;
  return gt4 ? ov_submit_poly_gt4 : gt3 ? ov_submit_poly_gt3 : 0;
}
// Scan a just-loaded overlay [base,base+size) for submit-library fns. The packet-pool load
// (lui $r,0x800C ; lw $r2,-0xABC(...)) appears only in the submit fns; for each, backtrack to the
// function entry (after the previous fn's `jr $ra`+delay slot) and classify+register there.
static void engine_scan_overlay(uint32_t base, uint32_t size) {
  uint32_t lo = base & ~3u, hi = (base + size) & ~3u;
  for (uint32_t a = lo; a + 4 <= hi; a += 4) {
    if ((mem_r32(a) >> 26) != 0x0Fu || (mem_r32(a) & 0xFFFFu) != 0x800Cu) continue;   // lui $r,0x800C
    if ((mem_r32(a + 4) >> 26) != 0x23u || (mem_r32(a + 4) & 0xFFFFu) != 0xF544u) continue; // lw ...,0xF544
    uint32_t entry = lo;                     // backtrack to the fn entry = just past previous `jr $ra`
    for (uint32_t b = a; b > lo && b > a - 64; b -= 4)
      if (mem_r32(b - 4) == 0x03E00008u) { entry = b + 4; break; }  // (b-4)=jr ra, (b)=delay slot, entry=b+4
    OverrideFn fn = classify_submit(entry);
    if (fn) {
      rec_set_interp_override_auto(entry, fn);
      if (cfg_dbg("submit"))
        fprintf(stderr, "[submit] own overlay %s @ 0x%08X (in load 0x%08X+0x%X)\n",
                fn == ov_submit_poly_gt4 ? "GT4" : "GT3", entry, base, size);
    }
  }
}
void engine_submit_register_autodetect(void) {
  if (submit_recomp()) return;               // A/B: keep all overlay submitters interpreted too
  if (cfg_on("PSXPORT_NO_OVERLAY_OWN")) return;   // A/B: measure overlay-ownership depth contribution
  rec_set_overlay_load_hook(engine_scan_overlay);
}
