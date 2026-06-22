// engine/gte.cpp — PC-native GTE matrix/vector transforms.

#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_syscall(Core*, uint32_t);

// 0x80084250 — transform 3 vertices through a GTE matrix (5 ctrl regs from a0), three GTE ops with
// the packed SXY/SZ vertex data at a1, and pack the resulting screen coords back into a0 (+0/+4/+8/
// +12/+16). Result-read order preserved (each op's data regs read before the next op overwrites them).
static void ov_80084250(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  gte_write_ctrl(0, c->mem_r32(a0 + 0)); gte_write_ctrl(1, c->mem_r32(a0 + 4)); gte_write_ctrl(2, c->mem_r32(a0 + 8));
  gte_write_ctrl(3, c->mem_r32(a0 + 12)); gte_write_ctrl(4, c->mem_r32(a0 + 16));
  uint32_t r8, r9, r10;
  r8 = c->mem_r16(a1 + 0); r9 = c->mem_r32(a1 + 4) & 0xFFFF0000u; r10 = c->mem_r32(a1 + 12);
  gte_write_data(0, r8 | r9); gte_write_data(1, r10); gte_op(c, 0x4A486012u);
  r8 = c->mem_r16(a1 + 2); r9 = c->mem_r32(a1 + 8) << 16; r10 = (uint32_t)(int16_t)c->mem_r16(a1 + 14);
  uint32_t r11 = gte_read_data(9), r12 = gte_read_data(10), r13 = gte_read_data(11);
  gte_write_data(0, r8 | r9); gte_write_data(1, r10); gte_op(c, 0x4A486012u);
  r8 = c->mem_r16(a1 + 4); r9 = c->mem_r32(a1 + 8) & 0xFFFF0000u; r10 = c->mem_r32(a1 + 16);
  uint32_t r14 = gte_read_data(9), r15 = gte_read_data(10), r24 = gte_read_data(11);
  gte_write_data(0, r8 | r9); gte_write_data(1, r10); gte_op(c, 0x4A486012u);
  c->mem_w32(a0 + 0, (r14 << 16) | (r11 & 0xFFFFu));
  c->mem_w32(a0 + 12, (r24 << 16) | (r13 & 0xFFFFu));
  uint32_t d9 = gte_read_data(9), d10 = gte_read_data(10);
  c->mem_w32(a0 + 4, (r12 << 16) | (d9 & 0xFFFFu));
  c->mem_w32(a0 + 8, (d10 << 16) | (r15 & 0xFFFFu));
  c->mem_w32(a0 + 16, gte_read_data(11));
  c->r[2] = a0;
}

// 0x80084470 — GTE matrix×vector: load 5 ctrl regs from the matrix at a0, 2 data regs from the vector
// at a1, run GTE op 0x4A486012, store IR-pack results (data regs 25/26/27) to a2; return a2.
static void ov_80084470(Core* c) {
  uint32_t m = c->r[4], v = c->r[5], o = c->r[6];
  gte_write_ctrl(0, c->mem_r32(m + 0));  gte_write_ctrl(1, c->mem_r32(m + 4));  gte_write_ctrl(2, c->mem_r32(m + 8));
  gte_write_ctrl(3, c->mem_r32(m + 12)); gte_write_ctrl(4, c->mem_r32(m + 16));
  gte_write_data(0, c->mem_r32(v + 0));  gte_write_data(1, c->mem_r32(v + 4));
  gte_op(c, 0x4A486012u);
  c->mem_w32(o + 0, gte_read_data(25)); c->mem_w32(o + 4, gte_read_data(26)); c->mem_w32(o + 8, gte_read_data(27));
  c->r[2] = o;
}

// 0x800847B0 — reformat a {int,int} pair pack from a0 into a1: each 32-bit field is stored as a word
// and then its low half-word is overwritten with a 16-bit value (a fixed-point pack). Order preserved
// from the recompiled body (the w16 fixups intentionally follow the w32 stores).
static void ov_800847B0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t r9 = c->mem_r32(a0 + 0), r10 = c->mem_r32(a0 + 4);
  c->mem_w32(a1 + 4, r9); c->mem_w32(a1 + 0, r10); c->mem_w16(a1 + 0, (uint16_t)r9);
  uint32_t r11 = c->mem_r32(a0 + 8), r9b = c->mem_r32(a0 + 12);
  c->mem_w32(a1 + 12, r11); c->mem_w32(a1 + 8, r9b);
  c->mem_w16(a1 + 12, (uint16_t)r10); c->mem_w16(a1 + 8, (uint16_t)r11);
  int16_t r10b = (int16_t)c->mem_r16(a0 + 16);
  c->mem_w16(a1 + 4, (uint16_t)r9b); c->mem_w16(a1 + 16, (uint16_t)r10b);
}

