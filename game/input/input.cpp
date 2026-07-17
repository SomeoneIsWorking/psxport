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

// ================================================================================================
// FUN_80093650 — one-shot voice/channel-table init. Zeroes the SPU-voice allocation table + the
// controller-channel state block at base 0x80100000 (gen: `32784u<<16`) + ~21688..24000, clamps the
// active-voice cap (arg0/r4, an int8) to <=24, then per-voice seeds the record (gain 255, pan/env
// defaults) and fires the SPU key-off / channel-reset helpers. ready-FRAME: mirrors the gen 112-byte
// stack frame (spills r16..r21 + r31 at sp+80..104, marshals a per-voice struct at sp+16 for
// func_80099970). Installed by guest address into the ONE override registry; gen_func_80093650 is the
// oracle leg (SBS core B). Body byte-faithful to the gen (tools/port_check.py gates it).
// ================================================================================================
#include "override_registry.h"   // overrides::install — the one native-override registry
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80093650(Core*);
void func_80099450(Core*);   // generated/shard_disp.c — SPU/voice subsystem reset
void func_80097760(Core*);   // generated/shard_disp.c
void func_80099970(Core*);   // generated/shard_disp.c — per-voice SPU key-off / envelope submit
void func_80094B50(Core*);   // generated/shard_disp.c — channel reset
void func_800931C0(Core*);   // generated/shard_disp.c — input processor prep

// ORACLE: gen_func_80093650
// PORT_GEN: 0x80093650 generated/shard_2.c:12938-13151
void Input::voiceTableInit(Core* c) {
  c->r[29] = c->r[29] + (uint32_t)-112;
  c->mem_w32((c->r[29] + (uint32_t)84), c->r[17]);
  c->r[17] = c->r[4] + c->r[0];
  c->r[4] = c->r[0] + c->r[0];
  c->mem_w32((c->r[29] + (uint32_t)104), c->r[31]);
  c->mem_w32((c->r[29] + (uint32_t)100), c->r[21]);
  c->mem_w32((c->r[29] + (uint32_t)96), c->r[20]);
  c->mem_w32((c->r[29] + (uint32_t)92), c->r[19]);
  c->mem_w32((c->r[29] + (uint32_t)88), c->r[18]);
  c->r[31] = 0x8009367Cu;
  c->mem_w32((c->r[29] + (uint32_t)80), c->r[16]); func_80099450(c);
  c->r[5] = (uint32_t)32784u << 16;
  c->r[5] = c->r[5] + (uint32_t)24000;
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)23696), (uint16_t)c->r[0]);
  c->r[31] = 0x80093694u;
  c->r[4] = c->r[0] + (uint32_t)32; func_80097760(c);
  c->r[16] = c->r[0] + c->r[0];
  c->r[3] = (uint32_t)32784u << 16;
  c->r[3] = c->r[3] + (uint32_t)23080;
  c->r[2] = c->r[16] & 65535u;
  L_800936A4:;
  c->r[2] = c->r[2] << 1;
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((c->r[2] + (uint32_t)0), (uint16_t)c->r[0]);
  c->r[16] = c->r[16] + (uint32_t)1;
  c->r[2] = c->r[16] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)192);
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[16] & 65535u; if (_t) goto L_800936A4; }
  c->r[16] = c->r[0] + c->r[0];
  c->r[2] = c->r[16] & 65535u;
  L_800936CC:;
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w8((c->r[1] + (uint32_t)23048), (uint8_t)c->r[0]);
  c->r[16] = c->r[16] + (uint32_t)1;
  c->r[2] = c->r[16] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)24);
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[16] & 65535u; if (_t) goto L_800936CC; }
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)23920), (uint16_t)c->r[0]);
  c->r[16] = c->r[0] + c->r[0];
  c->r[2] = c->r[16] & 65535u;
  L_800936FC:;
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w8((c->r[1] + (uint32_t)23832), (uint8_t)c->r[0]);
  c->r[16] = c->r[16] + (uint32_t)1;
  c->r[2] = c->r[16] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)16);
  { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[16] & 65535u; if (_t) goto L_800936FC; }
  c->r[2] = c->r[17] << 24;
  c->r[3] = (uint32_t)((int32_t)c->r[2] >> 24);
  c->r[2] = (uint32_t)(c->r[3] < (uint32_t)24);
  { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)6u << 16; if (_t) goto L_80093744; }
  c->r[2] = c->r[0] + (uint32_t)24;
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w8((c->r[1] + (uint32_t)23788), (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)6u << 16; goto L_8009374C;
  L_80093744:;
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w8((c->r[1] + (uint32_t)23788), (uint8_t)c->r[3]);
  L_8009374C:;
  c->r[2] = c->r[2] | 147u;
  c->r[3] = (uint32_t)32784u << 16;
  c->r[3] = (uint32_t)(int8_t)c->mem_r8((c->r[3] + (uint32_t)23788));
  c->r[16] = c->r[0] + c->r[0];
  c->mem_w32((c->r[29] + (uint32_t)20), c->r[2]);
  c->r[2] = c->r[0] + (uint32_t)4096;
  c->mem_w16((c->r[29] + (uint32_t)36), (uint16_t)c->r[2]);
  c->r[2] = c->r[0] + (uint32_t)4096;
  c->mem_w32((c->r[29] + (uint32_t)44), c->r[2]);
  c->r[2] = c->r[0] | 33023u;
  c->mem_w16((c->r[29] + (uint32_t)74), (uint16_t)c->r[2]);
  c->r[2] = c->r[0] + (uint32_t)16384;
  c->mem_w16((c->r[29] + (uint32_t)24), (uint16_t)c->r[0]);
  c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[0]);
  { int _t = ((int32_t)c->r[3] <= 0); c->mem_w16((c->r[29] + (uint32_t)76), (uint16_t)c->r[2]); if (_t) goto L_80093900; }
  c->r[21] = c->r[0] + (uint32_t)24;
  c->r[17] = c->r[0] + (uint32_t)255;
  c->r[20] = c->r[0] + (uint32_t)-1;
  c->r[19] = c->r[0] + (uint32_t)64;
  c->r[18] = c->r[0] + (uint32_t)1;
  c->r[4] = c->r[29] + (uint32_t)16;
  L_800937A4:;
  c->r[3] = c->r[16] & 65535u;
  c->r[2] = c->r[3] << 3;
  c->r[2] = c->r[2] - c->r[3];
  c->r[2] = c->r[2] << 3;
  c->r[3] = c->r[18] << (c->r[3] & 31);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21706), (uint16_t)c->r[21]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21704), (uint16_t)c->r[17]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w8((c->r[1] + (uint32_t)21733), (uint8_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21708), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21710), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21720), (uint16_t)c->r[20]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21722), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21724), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21726), (uint16_t)c->r[17]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21712), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21716), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w8((c->r[1] + (uint32_t)21714), (uint8_t)c->r[19]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21758), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21734), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21736), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21738), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21740), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21746), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21748), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21750), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21752), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21754), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[1] = c->r[1] + c->r[2];
  c->mem_w16((c->r[1] + (uint32_t)21742), (uint16_t)c->r[0]);
  c->r[31] = 0x800938D4u;
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[3]); func_80099970(c);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)23824), (uint16_t)c->r[16]);
  c->r[31] = 0x800938E4u;
  c->r[4] = c->r[0] + (uint32_t)1; func_80094B50(c);
  c->r[16] = c->r[16] + (uint32_t)1;
  c->r[3] = (uint32_t)32784u << 16;
  c->r[3] = (uint32_t)(int8_t)c->mem_r8((c->r[3] + (uint32_t)23788));
  c->r[2] = c->r[16] & 65535u;
  c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
  { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[29] + (uint32_t)16; if (_t) goto L_800937A4; }
  L_80093900:;
  c->r[2] = (uint32_t)32784u << 16;
  c->r[2] = c->r[2] + (uint32_t)23544;
  c->r[3] = c->r[0] + (uint32_t)16383;
  c->mem_w32((c->r[2] + (uint32_t)0), c->r[0]);
  c->mem_w16((c->r[2] + (uint32_t)8), (uint16_t)c->r[3]);
  c->mem_w16((c->r[2] + (uint32_t)10), (uint16_t)c->r[3]);
  c->mem_w32((c->r[2] + (uint32_t)4), c->r[0]);
  c->r[2] = c->r[0] + (uint32_t)128;
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)21688), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)21690), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)23536), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)21692), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)21694), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)21696), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)21698), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w8((c->r[1] + (uint32_t)23848), (uint8_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w16((c->r[1] + (uint32_t)23768), (uint16_t)c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->mem_w32((c->r[1] + (uint32_t)23792), c->r[0]);
  c->r[1] = (uint32_t)32784u << 16;
  c->r[31] = 0x8009397Cu;
  c->mem_w16((c->r[1] + (uint32_t)23770), (uint16_t)c->r[2]); func_800931C0(c);
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)104));
  c->r[21] = c->mem_r32((c->r[29] + (uint32_t)100));
  c->r[20] = c->mem_r32((c->r[29] + (uint32_t)96));
  c->r[19] = c->mem_r32((c->r[29] + (uint32_t)92));
  c->r[18] = c->mem_r32((c->r[29] + (uint32_t)88));
  c->r[17] = c->mem_r32((c->r[29] + (uint32_t)84));
  c->r[16] = c->mem_r32((c->r[29] + (uint32_t)80));
  c->r[29] = c->r[29] + (uint32_t)112; return;
  return;
}

// eov_voiceTableInit — guest-ABI thunk (arg0 in r4; the body leaves r2 as the gen epilogue does).
static void eov_voiceTableInit(Core* c) { Input::voiceTableInit(c); }

void Input::registerOverrides(Game* /*game*/) {
  overrides::install(0x80093650u, "Input::voiceTableInit", eov_voiceTableInit,
                     gen_func_80093650, shard_set_override);
}
