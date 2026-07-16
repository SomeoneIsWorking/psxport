// game/math/wide_re_gte_transform3.cpp — WIDE-RE DRAFT native ownership of 0x80084250 (FUN_80084250),
// a GTE 3-vertex rotate-and-pack utility that sits immediately after the owned Math cluster (game/math/
// gte_math.{h,cpp}: matMul=0x80084110, applyMatlv=0x80084220 end ~0x8008424C, applyMatrixLV=
// 0x80084470) — the "family resemblance" the task brief called out. Wide-RE tier (docs/
// fleet-workflow.md §6): UNWIRED / UNVERIFIED, hand-transliterated 1:1 from generated/shard_0.c
// gen_func_80084250 (66 gen-C ln) — ground truth, NOT mechanically diffed yet. Not called from
// anywhere (no override-registry registration, no shard_set_override) — dead code that only needs to
// COMPILE. A wiring pass MUST re-diff every line against the generated C before registering +
// SBS-gating (per §9).
//
// Shape (MEDIUM confidence — register-flow is exact, source-struct field ROLES are inferred from
// operand width/shift patterns only, never confirmed against a live dump):
//   a0 (r4) = a 5-word (20 B) buffer that is BOTH the input rotation matrix (read at entry, loaded
//     into GTE CR0-4 via gte_write_ctrl, CR-packed layout matching Math::matMul/rotmat's outPtr
//     format) AND the output buffer — its own 5 words get OVERWRITTEN in place with the packed
//     per-vertex transform results before return (same address used for both reads-at-entry and
//     writes-at-exit; verified from the gen body, not a guess).
//   a1 (r5) = a 20-byte, 3-vertex SoA source array, inferred layout (u16 unless noted):
//       +0 X0, +2 X1, +4 X2 (low16 of the word @+4), +6 Y0 (high16 of the word @+4),
//       +8 Y1 (low16 of the word @+8), +10 Y2 (high16 of the word @+8),
//       +12 VZ0 (s32, already GTE-VZ-packed — copied to gte data reg 1 verbatim),
//       +14 Z1 (s16, sign-extended by this function before use),
//       +16 VZ2 (s32, already GTE-VZ-packed — copied verbatim, like VZ0).
//     (Word @+4 packs X2 low / Y0 high; word @+8 packs Y1 low / Y2 high — confirmed by two DISTINCT
//     reads of the SAME source word with different mask/shift per vertex, not two separate fields.)
//
// Algorithm: gte_write_ctrl(0..4) load the rotation matrix from a0. Then for each of the 3 vertices,
// write GTE data regs 0/1 (VXY/VZ) from the a1 fields above and run RTPS (gte_op 0x4A486012 — the
// 6-bit COP2 function code for perspective-transform-single, matching the doc's "GTE ops actually
// used: RTPS/RTPT"). BEFORE writing each NEXT vertex's data (not after — this is a genuine
// pipelining quirk of the guest code, transcribed as-is), the PREVIOUS vertex's transformed IR1/2/3
// (GTE data regs 9/10/11) are read back and held. After all 3 RTPS calls, the 9 held IR values (3
// per vertex) are packed pairwise into 4 output words (hi16=one vertex's component, lo16=another's)
// plus vertex2's IR3 written whole — the exact interleaving the gen body performs, preserved 1:1
// below. Output written back into a0[0..16] (in place).
#include "core.h"
#include <stdint.h>

// gte_write_ctrl/gte_write_data/gte_read_data/gte_op declared in core.h.

static void func_80084250(Core* c) {
  uint32_t mat = c->r[4];   // in/out matrix buffer (a0)
  uint32_t src = c->r[5];   // 3-vertex source array (a1)

  gte_write_ctrl(0, c->mem_r32(mat + 0));
  gte_write_ctrl(1, c->mem_r32(mat + 4));
  gte_write_ctrl(2, c->mem_r32(mat + 8));
  gte_write_ctrl(3, c->mem_r32(mat + 12));
  gte_write_ctrl(4, c->mem_r32(mat + 16));

  // --- vertex 0 ---
  uint32_t vxy0 = (uint32_t)c->mem_r16(src + 0);           // X0, zero-extended
  uint32_t y0hi = c->mem_r32(src + 4) & 0xFFFF0000u;        // Y0 packed in the high 16 of word@+4
  vxy0 |= y0hi;
  uint32_t vz0 = c->mem_r32(src + 12);                      // already GTE-VZ-packed
  gte_write_data(0, vxy0);
  gte_write_data(1, vz0);
  gte_op(c, 0x4A486012u);   // RTPS vertex0

  // --- vertex 1 (reads vertex0's IR1/2/3 BEFORE writing vertex1's data — pipelined as in the gen body) ---
  uint32_t vxy1 = (uint32_t)c->mem_r16(src + 2);           // X1, zero-extended
  uint32_t y1lo = (c->mem_r32(src + 8) << 16);              // Y1 packed in the LOW 16 of word@+8, shifted to hi16 here (matches gen: r9<<16)
  vxy1 |= y1lo;
  int32_t vz1 = c->mem_r16s(src + 14);                      // Z1, sign-extended s16 -> s32
  uint32_t ir1_v0 = gte_read_data(9);
  uint32_t ir2_v0 = gte_read_data(10);
  uint32_t ir3_v0 = gte_read_data(11);
  gte_write_data(0, vxy1);
  gte_write_data(1, (uint32_t)vz1);
  gte_op(c, 0x4A486012u);   // RTPS vertex1

  // --- vertex 2 (reads vertex1's IR1/2/3 BEFORE writing vertex2's data) ---
  uint32_t vxy2 = (uint32_t)c->mem_r16(src + 4);            // "X2" = low16 of the SAME word@+4 vertex0 read as Y0's high16
  uint32_t y2hi = c->mem_r32(src + 8) & 0xFFFF0000u;         // Y2 = high16 of the SAME word@+8 vertex1 read as Y1's low16
  vxy2 |= y2hi;
  uint32_t vz2 = c->mem_r32(src + 16);                       // already GTE-VZ-packed
  uint32_t ir1_v1 = gte_read_data(9);
  uint32_t ir2_v1 = gte_read_data(10);
  uint32_t ir3_v1 = gte_read_data(11);
  gte_write_data(0, vxy2);
  gte_write_data(1, vz2);
  gte_op(c, 0x4A486012u);   // RTPS vertex2

  // --- pack results back into mat[0..16] (in place) ---
  uint32_t out0 = (ir1_v0 & 0xFFFFu) | (ir1_v1 << 16);
  c->mem_w32(mat + 0, out0);

  uint32_t out12 = (ir3_v0 & 0xFFFFu) | (ir3_v1 << 16);
  c->mem_w32(mat + 12, out12);

  uint32_t ir1_v2 = gte_read_data(9);
  uint32_t ir2_v2 = gte_read_data(10);
  uint32_t out4 = (ir1_v2 & 0xFFFFu) | (ir2_v0 << 16);
  c->mem_w32(mat + 4, out4);

  uint32_t out8 = (ir2_v1 & 0xFFFFu) | (ir2_v2 << 16);
  c->mem_w32(mat + 8, out8);

  c->mem_w32(mat + 16, gte_read_data(11));  // vertex2's IR3, whole word, unpacked

  c->r[2] = mat;  // return value = the same buffer pointer (a0)
}
