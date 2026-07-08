// game/render/quad_rtpt_submit.cpp — see quad_rtpt_submit.h. Faithful substrate-mirror DRAFTS of
// FUN_8003B054 and FUN_8003B320, RE'd instruction-by-instruction from the recompiler's own
// translation (generated/shard_3.c gen_func_8003B054, generated/shard_6.c gen_func_8003B320 —
// ground truth per CLAUDE.md for GTE-bearing code; Ghidra's COP2 decompile of FUN_8003B320 renders
// the GTE data-register writes as synthetic setCopReg/getCopReg/copFunction "bus" pseudo-calls that
// do not resolve to plain register indices, so it was cross-checked against, not relied on, for the
// GTE portion — FUN_8003B054 has no GTE so Ghidra's decompile of it was already reliable and is
// reproduced 1:1 below). UNWIRED, UNVERIFIED — no SBS run per this fleet-agent's instructions.
#include "quad_rtpt_submit.h"
#include "core.h"
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8003B054 — rotate the 4 corner fields of a quad record from `src` into `dst`'s reserved
// vertex-index/extent slots (+0xC/+0x14/+0x1C/+0x24, each a u16; a shared 2nd word at +0xE/+0x16
// filled from src+2/src+6 in the SAME order for every non-zero index). `idx` selects which
// physical corner of `src` becomes dst's "first" corner (a cyclic rotation) and, for idx 1..3,
// applies a small "-1" shrink to specific BYTES of the 4 written u16s (idx1: low byte of all 4;
// idx2: high byte of all 4; idx3: both bytes of all 4) — idx 0 does no byte adjustment and writes
// all 4 fields as FULL 32-bit words (src's next word, not just its low u16) with an early return
// that SKIPS the shared +0xE/+0x16 tail (traced exactly from gen_func_8003B054's control flow —
// this asymmetry is real, not an RE artifact: idx0 is qualitatively different from 1/2/3).
void QuadRtptSubmit::rotateQuadCorners(Core* c) {
  const uint32_t dst = c->r[4];   // a0
  const uint32_t src = c->r[5];   // a1
  const int32_t  idx = (int32_t)c->r[6];   // a2: corner/orientation selector

  if (idx == 1) {
    c->mem_w16(dst + 0x0C, c->mem_r16(src + 4));
    c->mem_w16(dst + 0x14, c->mem_r16(src + 0));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 12));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 8));
    c->mem_w8(dst + 0x0C, (uint8_t)(c->mem_r8(dst + 0x0C) - 1));   // low byte of each corner -1
    c->mem_w8(dst + 0x14, (uint8_t)(c->mem_r8(dst + 0x14) - 1));
    c->mem_w8(dst + 0x1C, (uint8_t)(c->mem_r8(dst + 0x1C) - 1));
    c->mem_w8(dst + 0x24, (uint8_t)(c->mem_r8(dst + 0x24) - 1));
  } else if (idx < 2) {
    if (idx != 0) return;
    c->mem_w32(dst + 0x0C, c->mem_r32(src + 0));
    c->mem_w32(dst + 0x14, c->mem_r32(src + 4));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 8));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 12));
    return;   // idx==0 skips the shared tail below — faithful to gen_func_8003B054
  } else if (idx == 2) {
    c->mem_w16(dst + 0x0C, c->mem_r16(src + 8));
    c->mem_w16(dst + 0x14, c->mem_r16(src + 12));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 0));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 4));
    c->mem_w8(dst + 0x0D, (uint8_t)(c->mem_r8(dst + 0x0D) - 1));   // high byte of each corner -1
    c->mem_w8(dst + 0x15, (uint8_t)(c->mem_r8(dst + 0x15) - 1));
    c->mem_w8(dst + 0x1D, (uint8_t)(c->mem_r8(dst + 0x1D) - 1));
    c->mem_w8(dst + 0x25, (uint8_t)(c->mem_r8(dst + 0x25) - 1));
  } else {
    if (idx != 3) return;
    c->mem_w16(dst + 0x0C, c->mem_r16(src + 12));
    c->mem_w16(dst + 0x14, c->mem_r16(src + 8));
    c->mem_w16(dst + 0x1C, c->mem_r16(src + 4));
    c->mem_w16(dst + 0x24, c->mem_r16(src + 0));
    // both bytes of each of the 4 corners -1 (two independent byte-decrements each, NOT a u16 -=1 —
    // matches gen_func_8003B054's per-byte store order exactly, borrow behaviour included).
    c->mem_w8(dst + 0x0C, (uint8_t)(c->mem_r8(dst + 0x0C) - 1));
    c->mem_w8(dst + 0x0D, (uint8_t)(c->mem_r8(dst + 0x0D) - 1));
    c->mem_w8(dst + 0x14, (uint8_t)(c->mem_r8(dst + 0x14) - 1));
    c->mem_w8(dst + 0x15, (uint8_t)(c->mem_r8(dst + 0x15) - 1));
    c->mem_w8(dst + 0x1C, (uint8_t)(c->mem_r8(dst + 0x1C) - 1));
    c->mem_w8(dst + 0x1D, (uint8_t)(c->mem_r8(dst + 0x1D) - 1));
    c->mem_w8(dst + 0x24, (uint8_t)(c->mem_r8(dst + 0x24) - 1));
    c->mem_w8(dst + 0x25, (uint8_t)(c->mem_r8(dst + 0x25) - 1));
  }

  // shared tail (idx 1/2/3 only): second word's two halves, UNROTATED (always src+2/src+6).
  c->mem_w16(dst + 0x16, c->mem_r16(src + 6));
  c->mem_w16(dst + 0x0E, c->mem_r16(src + 2));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8003B320 — project a quad through an already-composed GTE transform (RTPT the first 3
// corners, RTPS the 4th) and, if it survives the on-screen + OT-range gates, bump-copy the
// pre-built 10-word packet record into the packet pool and link it into the OT bucket for its
// depth. Traced from gen_func_8003B320 (generated/shard_6.c) — same gte_op idiom as the already-
// owned OverlayGt3Gt4::gt3/gt4 (game/render/overlay_gt3gt4.cpp), which this mirrors closely: RTPT
// via 0x4A280030, RTPS via 0x4A180001, AVSZ4 via 0x4B68002E, OTZ-bucket compute identical to
// overlay_gt_otz_index (z>>10 exponent-shift index, valid range [4,0x7ff]). UNLIKE the overlay
// leaves there is NO NCLIP/backface test here — every on-screen, in-range quad is drawn.
//
// out            (a0): 10-word (40-byte) packet record. +4/+12/+20/+28/+36 = colour/uv/etc already
//                       filled in by the caller (verbatim-copied, this leaf never reads their
//                       meaning); +8/+16/+24/+32 = the 4 SXY slots THIS leaf fills via RTPT/RTPS.
// composedXform  (a1): 6 packed words = GTE VXY0/VZ0/VXY1/VZ1/VXY2/VZ2 (MTC2-ready model-space
//                       corner data for the RTPT), plus 2 more words at +24/+28 = VXY3/VZ3 for the
//                       RTPS 4th corner. The caller composes this (un-RE'd, outside this band —
//                       see quad_rtpt_submit.h); this leaf only consumes it.
// otzBias        (a2): added to the AVSZ4 result before the OT-bucket index is derived.
void QuadRtptSubmit::submitQuad(Core* c) {
  const uint32_t out = c->r[4];              // a0
  const uint32_t xf  = c->r[5];               // a1: composedXform
  const int32_t  otzBias = (int32_t)c->r[6];  // a2

  gte_write_data(0, c->mem_r32(xf + 0));      // VXY0
  gte_write_data(1, c->mem_r32(xf + 4));      // VZ0
  gte_write_data(2, c->mem_r32(xf + 8));      // VXY1
  gte_write_data(3, c->mem_r32(xf + 12));     // VZ1
  gte_write_data(4, c->mem_r32(xf + 16));     // VXY2
  gte_write_data(5, c->mem_r32(xf + 20));     // VZ2
  gte_op(c, 0x4A280030u);                     // RTPT (corners 0..2)
  if ((int32_t)gte_read_ctrl(31) < 0) return; // GTE FLAG error -> drop the whole quad

  c->mem_w32(out + 8,  gte_read_data(12));    // SXY0
  c->mem_w32(out + 16, gte_read_data(13));    // SXY1
  c->mem_w32(out + 24, gte_read_data(14));    // SXY2

  gte_write_data(0, c->mem_r32(xf + 24));     // VXY3
  gte_write_data(1, c->mem_r32(xf + 28));     // VZ3
  gte_op(c, 0x4A180001u);                     // RTPS (corner 3)
  if ((int32_t)gte_read_ctrl(31) < 0) return; // GTE FLAG error -> drop
  c->mem_w32(out + 32, gte_read_data(14));    // SXY3

  gte_op(c, 0x4B68002Eu);                     // AVSZ4
  int32_t z0 = (int32_t)gte_read_data(7);
  if (z0 < 0) return;                          // raw AVSZ4 error -> drop (checked BEFORE bias, faithful)
  int32_t z = z0 + otzBias;
  if (z < 0) return;                           // biased z still must be non-negative

  int32_t shift = z >> 10;
  int32_t otz = (z >> (shift & 31)) + shift * 0x200;
  if (otz < 4 || otz > 0x7FF) return;           // faithful range gate: valid index is [4, 0x7FF]

  // on-screen test: ALL 4 corners' SX in [0,320) AND ALL 4 corners' SY in [0,240) — unsigned 16-bit
  // compares, so a negative (wrapped) coordinate also fails. Faithful 4:3-only frustum (see header).
  auto sx = [&](uint32_t off) { return (uint16_t)c->mem_r16(out + off); };
  bool xok = sx(8) < 320 && sx(16) < 320 && sx(24) < 320 && sx(32) < 320;
  if (!xok) return;
  bool yok = sx(10) < 240 && sx(18) < 240 && sx(26) < 240 && sx(34) < 240;
  if (!yok) return;

  // bump-copy the whole 10-word record into the packet pool, OT-link it.
  const uint32_t POOL_PTR = 0x800BF544u;
  const uint32_t OT_BASE_PTR = 0x800ED8C8u;
  uint32_t pool = c->mem_r32(POOL_PTR);
  uint32_t otbase = c->mem_r32(OT_BASE_PTR);
  uint32_t slot = otbase + (uint32_t)otz * 4;
  uint32_t old_head = c->mem_r32(slot);
  c->mem_w32(pool, old_head | (9u << 24));    // tag: len=9 data words | old OT head
  c->mem_w32(slot, pool);                     // OT bucket head = this packet
  uint32_t dstw = pool + 4;
  for (uint32_t off = 4; off <= 36; off += 4, dstw += 4)
    c->mem_w32(dstw, c->mem_r32(out + off));
  c->mem_w32(POOL_PTR, pool + 40);
}
