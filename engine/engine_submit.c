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

void gen_func_8007FDB0(R3000*);   // recomp body (A/B oracle / super-call)
void gen_func_8008007C(R3000*);   // recomp body (quad)

#define PKT_POOL_PTR 0x800BF544u   // DAT_800bf544: current free GPU-packet write pointer
#define COL_MASK     0xFFF0F0F0u   // low-nibble-per-byte clear applied to packet RGB words

static int s_submit_recomp = -1;   // PSXPORT_SUBMIT_RECOMP=1 -> keep the recomp body (A/B)
static int submit_recomp(void) { if (s_submit_recomp < 0) s_submit_recomp = cfg_on("PSXPORT_SUBMIT_RECOMP") ? 1 : 0;
                                 return s_submit_recomp; }

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
static uint32_t ot_compress(uint32_t otz) {
  uint32_t sh = otz >> 10;
  uint32_t idx = (otz >> (sh & 31)) + (sh << 9);
  if ((uint32_t)(idx - 4) < 2044u) return idx;   // in range
  return 0xFFFFFFFFu;                              // out of range -> -1 -> skip
}

// gen_func_8007FDB0 — POLY_GT3 (gouraud-textured triangle) submit.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1 (rgb2 = rgb1<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2(hi)}.
// Packet = 40 bytes POLY_GT3: {tag, rgb0|code, SXY0, uv0|clut, rgb1, SXY1, uv1|tpage, rgb2, SXY2, uv2}.
static void submit_poly_gt3(R3000* c) {
  uint32_t rec = c->r[4], ot = c->r[5], count = c->r[6];
  uint32_t pkt = mem_r32(PKT_POOL_PTR);
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
    pkt += 40;
  }
  mem_w32(PKT_POOL_PTR, pkt);
  c->r[2] = rec;                                   // return: record pointer advanced past the array
}

void ov_submit_poly_gt3(R3000* c) {
  if (submit_recomp()) { gen_func_8007FDB0(c); return; }
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
    pkt += 52;
  }
  mem_w32(PKT_POOL_PTR, pkt);
  c->r[2] = rec;                                   // return: record pointer advanced past the array
}

void ov_submit_poly_gt4(R3000* c) {
  if (submit_recomp()) { gen_func_8008007C(c); return; }
  submit_poly_gt4(c);
}
