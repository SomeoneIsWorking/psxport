// game/input/input.cpp — PC-native per-frame INPUT/controller-state subsystem.
// The per-frame input / controller-state processor (FUN_800931C0) — five phases over the global
// controller tables (ring buffer, presence accumulator, coherence window, channel flushes). Control
// flow + memory ops owned native; every sub-call (incl. the indirect fn-ptr globals) stays reachable by
// address via rec_dispatch. We mirror the gen 120-byte stack frame so sub-calls' frames align. Extracted
// verbatim from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code
// structure. The `pad931c0` diagnostic A/B gate (full RAM+scratchpad vs rec_super_call) is a REPL
// channel, unchanged.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// FUN_800931C0 — per-frame INPUT/controller-state processor (the heaviest un-owned resident fn, ~12% of
// field time). Five phases over the global tables at 0x80105xxx / 0x801054xx:
//  P0  advance the 16-slot ring index 0x80105BAC (=(idx+1)&15), clear new ring slot 0x80105BB0[idx].
//  P1  for each object [0, (int8)0x80105CEC): jal 0x8009A1D0(s0, &rec[s0]) (rec base 0x801054CE, stride 56);
//      if rec[s0].h0==0 set bit s0 in ring slot 0x80105BB0[idx] (the "was-present this frame" accumulator).
//  P2  if (int8)0x80105D28==0: acc = AND of ring slots 0..14 (a 15-frame coherence window); for each object,
//      if (acc>>s0)&1 and rec-byte 0x801054E5[s0*56]==2 -> jal 0x80097E10(0, mask) (mask = (int16)(1<<s0)
//      for s0<16, else ((1<<(s0-16))&0xff)<<16); clear that byte.
//  P3  0x801054B8 &= ~0x80105BF0; 0x801054BA &= ~0x80105BF2; for s0 in [0,24): if h[0x801054E6+s0*56]!=0
//      call (*0x80105BA8)(s0); if h[0x801054F2+s0*56]!=0 call (*0x80105A20)(s0)  (indirect fn-ptr globals).
//  P4  for s0 in [0,24): marshal a struct on the guest stack (base sp+16) from the per-slot flag byte
//      0x80105A08[s0] + the halfword fields at 0x80105A28+s0*16, set field+4 bits (1->3, 4->|0x10,
//      8->|0x80, 0x10->|0x60000); if field+4 != 0 jal 0x80099970(struct); clear the flag byte.
//  P5  four channel flushes: 0x80098F90(0, bf2:bf0), 0x80098F90(1, 54ba:54b8), 0x80098DB0(8, 54be:54bc),
//      0x80097E10(8, 54c2:54c0); then zero 0x80105BF0/BF2 and 0x801054B8/BA/C0/C2.
// CONTROL FLOW + memory ops owned native; every jal (incl. the P3 indirect fn-ptrs) stays interpreted via
// rec_dispatch. We mirror the gen 120-byte stack frame (sp -= 120) so the P4 struct lands where 0x80099970
// expects it AND every sub-call's stack frame aligns with the gen body. `pad931c0` gate = full RAM+
// scratchpad A/B vs rec_super_call, excluding the fn's own frame [old_sp-120, old_sp). v0 carries the last
// sub-call's (0x80097E10) return, matching the gen epilogue.
static void input_dispatch_931c0(Core* c) {
  uint32_t old_sp = c->r[29];
  uint32_t sp = old_sp - 120;
  c->r[29] = sp;
  // P0: advance ring
  uint32_t ridx = (c->mem_r32(0x80105BACu) + 1) & 0xf;
  c->mem_w32(0x80105BACu, ridx);
  c->mem_w32(0x80105BB0u + ridx * 4, 0);
  // P1
  for (int s0 = 0; s0 < (int)c->mem_r8s(0x80105CECu); s0++) {
    uint32_t rec = 0x801054CEu + (uint32_t)s0 * 56;
    c->r[4] = (uint32_t)s0; c->r[5] = rec; rec_dispatch(c, 0x8009A1D0u);
    if (c->mem_r16(rec) == 0) {
      uint32_t a = 0x80105BB0u + c->mem_r32(0x80105BACu) * 4;
      c->mem_w32(a, c->mem_r32(a) | (1u << (s0 & 31)));
    }
  }
  // P2
  if (c->mem_r8s(0x80105D28u) == 0) {
    uint32_t acc = 0xFFFFFFFFu;
    for (int k = 0; k < 15; k++) acc &= c->mem_r32(0x80105BB0u + (uint32_t)k * 4);
    for (int s0 = 0; s0 < (int)c->mem_r8s(0x80105CECu); s0++) {
      if (acc & (1u << (s0 & 31))) {
        uint32_t recb = 0x801054E5u + (uint32_t)s0 * 56;
        if (c->mem_r8s(recb) == 2) {
          uint32_t a1 = (s0 < 16) ? (uint32_t)(int32_t)(int16_t)(uint16_t)(1u << s0)
                                  : (((1u << ((s0 - 16) & 31)) & 0xffu) << 16);
          c->r[4] = 0; c->r[5] = a1; rec_dispatch(c, 0x80097E10u);
        }
        c->mem_w8(recb, 0);
      }
    }
  }
  // P3
  c->mem_w16(0x801054B8u, (uint16_t)(c->mem_r16(0x801054B8u) & (uint16_t)~(uint16_t)c->mem_r16(0x80105BF0u)));
  c->mem_w16(0x801054BAu, (uint16_t)(c->mem_r16(0x801054BAu) & (uint16_t)~(uint16_t)c->mem_r16(0x80105BF2u)));
  for (int s0 = 0; s0 < 24; s0++) {
    uint32_t off = (uint32_t)s0 * 56;
    if (c->mem_r16(0x801054E6u + off) != 0) { c->r[4] = (uint32_t)s0; rec_dispatch(c, c->mem_r32(0x80105BA8u)); }
    if (c->mem_r16(0x801054F2u + off) != 0) { c->r[4] = (uint32_t)s0; rec_dispatch(c, c->mem_r32(0x80105A20u)); }
  }
  // P4
  for (int s0 = 0; s0 < 24; s0++) {
    uint32_t off = (uint32_t)s0 * 16;
    c->mem_w32(sp + 16, 1u << (s0 & 31));
    c->mem_w32(sp + 20, 0);
    uint32_t f20 = 0;
    uint8_t flags = c->mem_r8(0x80105A08u + (uint32_t)s0);
    if (flags & 1) {
      f20 = 3; c->mem_w32(sp + 20, f20);
      c->mem_w16(sp + 24, c->mem_r16(0x80105A28u + off));
      c->mem_w16(sp + 26, c->mem_r16(0x80105A2Au + off));
    }
    if (c->mem_r8(0x80105A08u + (uint32_t)s0) & 4) {
      f20 |= 0x10; c->mem_w32(sp + 20, f20);
      c->mem_w16(sp + 36, c->mem_r16(0x80105A2Cu + off));
    }
    if (c->mem_r8(0x80105A08u + (uint32_t)s0) & 8) {
      f20 |= 0x80; c->mem_w32(sp + 20, f20);
      c->mem_w32(sp + 44, (uint32_t)c->mem_r16(0x80105A2Eu + off) << 3);
    }
    if (c->mem_r8(0x80105A08u + (uint32_t)s0) & 0x10) {
      f20 |= 0x60000; c->mem_w32(sp + 20, f20);
      c->mem_w16(sp + 74, c->mem_r16(0x80105A30u + off));
      c->mem_w16(sp + 76, c->mem_r16(0x80105A32u + off));
    }
    if (c->mem_r32(sp + 20) != 0) { c->r[4] = sp + 16; rec_dispatch(c, 0x80099970u); }
    c->mem_w8(0x80105A08u + (uint32_t)s0, 0);
  }
  // P5
  c->r[4] = 0; c->r[5] = ((uint32_t)c->mem_r8(0x80105BF2u) << 16) | (uint32_t)c->mem_r16(0x80105BF0u); rec_dispatch(c, 0x80098F90u);
  c->r[4] = 1; c->r[5] = ((uint32_t)c->mem_r8(0x801054BAu) << 16) | (uint32_t)c->mem_r16(0x801054B8u); rec_dispatch(c, 0x80098F90u);
  c->r[4] = 8; c->r[5] = ((uint32_t)c->mem_r8(0x801054BEu) << 16) | (uint32_t)c->mem_r16(0x801054BCu); rec_dispatch(c, 0x80098DB0u);
  c->r[4] = 8; c->r[5] = ((uint32_t)c->mem_r8(0x801054C2u) << 16) | (uint32_t)c->mem_r16(0x801054C0u); rec_dispatch(c, 0x80097E10u);
  c->mem_w16(0x80105BF0u, 0); c->mem_w16(0x80105BF2u, 0);
  c->mem_w16(0x801054B8u, 0); c->mem_w16(0x801054BAu, 0);
  c->mem_w16(0x801054C0u, 0); c->mem_w16(0x801054C2u, 0);
  c->r[29] = old_sp;
}
