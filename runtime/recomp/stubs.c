// S1 stubs for runtime hooks not yet implemented (GTE, COP0, syscalls). These let the
// recompiled core compile and link so the emitter can be verified now; each is replaced
// by a real implementation in later stages (S2 HLE, S3 GTE).
#include "r3000.h"
#include <stdio.h>

static uint32_t g_gte_data[64], g_gte_ctrl[64];

uint32_t gte_read_data(uint32_t reg)            { return g_gte_data[reg & 63]; }
void     gte_write_data(uint32_t reg, uint32_t v) { g_gte_data[reg & 63] = v; }
uint32_t gte_read_ctrl(uint32_t reg)            { return g_gte_ctrl[reg & 63]; }
void     gte_write_ctrl(uint32_t reg, uint32_t v) { g_gte_ctrl[reg & 63] = v; }
void     gte_op(R3000* c, uint32_t insn)        { (void)c; (void)insn; }

uint32_t cop0_mfc(R3000* c, uint32_t reg)       { (void)c; (void)reg; return 0; }
void     cop0_mtc(R3000* c, uint32_t reg, uint32_t v) { (void)c; (void)reg; (void)v; }

void rec_syscall(R3000* c, uint32_t code) {
  (void)c;
  fprintf(stderr, "[syscall] code %u (unimplemented)\n", code);
}
void rec_break(R3000* c, uint32_t code) {
  (void)c;
  fprintf(stderr, "[break] code %u\n", code);
}
