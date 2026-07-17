// game/ai/beh_box_seed_phase_gate.cpp — PC-native per-object BEHAVIOR handler FUN_8012A0B8.
//
// Overlay handler (~x2334/field-frame on seaside; ~135 instr), prologue 0x8012A0B8; `jr ra` at
// 0x8012A2D0. Disassembled from scratch/ram/field_seaside.bin. Two-level state machine (outer
// node[4] = s0):
//   STATE 2/3 : FUN_8007A624(node).   STATE >=4 : nothing.
//   STATE 0   : INIT — node[11]=32, node[4]=1, node[8]=0, node[9]=0, node[0x18]=0x8013EA64; then
//               if node[3]<2: FUN_801360F4(node,node[3]) + FUN_80139838(node,0)/(node,1);
//               else        : FUN_8013AC34(node,node[3]);
//               then copy a per-node[3] record from table @0x80149EC4 (stride 10) into the node:
//                 node[0x80]=tbl[+2] node[0x82]=tbl[+6] node[0x84]=tbl[+4] node[0x86]=tbl[+8]
//                 node[0x2E]=node[0x4E]=(tbl[+2]+tbl[+6])/2   (round-toward-zero)
//                 node[0x32]=node[0x50]=node[0x52]=tbl[+0]
//                 node[0x36]=(tbl[+4]+tbl[+8])/2
//               then FUN_80129E8C(node, node[0x32]).
//   STATE 1   : read scratchpad byte 0x1F800207; if 25 <= b < 32 -> FUN_80129E8C(node,...) and node[1]=1.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf, a0/a1 set as the guest does).
// The per-node[3] table @0x80149EC4 lives in the resident overlay RAM — we READ it live (same memory
// the recomp reads); it is NOT embedded. Transcribed 1:1 as a register machine (locals = guest regs,
// goto labels = guest addresses); signed (lh/sra) vs unsigned (lhu/lbu) preserved exactly. The
// byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012A0B8u;

}  // namespace

void beh_box_seed_phase_gate(Core* c) {
  uint32_t s1 = c->r[4];                            // s1 = a0 (node)
  uint32_t s0 = c->mem_r8(s1 + 4);                  // s0 = node[4] = outer state

  if (s0 == 1) goto La28c;                          // STATE 1
  if ((int32_t)s0 < 2) { if (s0 == 0) goto La108; goto Lret; }  // s0<2 -> only s0==0
  if ((int32_t)s0 < 4) goto La2bc;                  // s0 in {2,3} -> FUN_8007A624
  goto Lret;                                        // s0 >= 4

 La108:                                             // STATE 0 — INIT
  {
    c->mem_w8(s1 + 11, 32);                         // node[11] = 32 (v0=32 from delay slot)
    c->mem_w8(s1 + 4, 1);                           // node[4] = 1
    uint8_t n3 = c->mem_r8(s1 + 3);                 // v1 = node[3]
    c->mem_w8(s1 + 8, 0);                           // node[8] = 0
    c->mem_w8(s1 + 9, 0);                           // node[9] = 0
    c->mem_w32(s1 + 0x18, 0x8013EA64u);             // node[0x18] = 0x8013EA64 (branch-delay store)

    if (n3 < 2) {
      guest_leaf(c, 0x801360F4u, s1, n3);                // FUN_801360F4(node, node[3])
      for (int i = 0; i < 2; i++)
        guest_leaf(c, 0x80139838u, s1, (uint32_t)i);     // FUN_80139838(node, 0), (node, 1)
    } else {
      guest_leaf(c, 0x8013AC34u, s1, n3);                // FUN_8013AC34(node, node[3])
    }

    // per-node[3] record copy: table @0x80149EC4, stride 10 bytes, element = base + node[3]*10
    uint32_t elem = 0x80149EC4u + (uint32_t)n3 * 10u;
    c->mem_w16(s1 + 0x80, c->mem_r16(elem + 2));    // node[0x80] = tbl[+2]
    c->mem_w16(s1 + 0x82, c->mem_r16(elem + 6));    // node[0x82] = tbl[+6]
    c->mem_w16(s1 + 0x84, c->mem_r16(elem + 4));    // node[0x84] = tbl[+4]
    c->mem_w16(s1 + 0x86, c->mem_r16(elem + 8));    // node[0x86] = tbl[+8]

    // node[0x2E] = node[0x4E] = (tbl[+2] + tbl[+6]) / 2  (signed, round toward zero)
    int32_t s_26 = c->mem_r16s(elem + 2) + c->mem_r16s(elem + 6);
    int32_t avg1 = (int32_t)(((uint32_t)s_26 + ((uint32_t)s_26 >> 31)) >> 1);  // sra((x + signbit),1)
    c->mem_w16(s1 + 0x2E, (uint16_t)avg1);
    c->mem_w16(s1 + 0x32, c->mem_r16(elem + 0));    // node[0x32] = tbl[+0]
    c->mem_w16(s1 + 0x4E, c->mem_r16(s1 + 0x2E));   // node[0x4E] = node[0x2E]

    // node[0x36] = (tbl[+4] + tbl[+8]) / 2  (signed, round toward zero)
    int32_t s_36 = c->mem_r16s(elem + 4) + c->mem_r16s(elem + 8);
    int32_t avg2 = (int32_t)(((uint32_t)s_36 + ((uint32_t)s_36 >> 31)) >> 1);
    uint16_t v32 = c->mem_r16(s1 + 0x32);           // node[0x32]
    c->mem_w16(s1 + 0x36, (uint16_t)avg2);
    c->mem_w16(s1 + 0x50, v32);                     // node[0x50] = node[0x32]
    c->mem_w16(s1 + 0x52, v32);                     // node[0x52] = node[0x32]  (delay slot store)
    guest_leaf(c, 0x80129E8Cu, s1, (uint32_t)v32);       // FUN_80129E8C(node, node[0x32])
  }
  goto Lret;

 La28c:                                             // STATE 1
  {
    uint8_t b = c->mem_r8(0x1F800207u);             // scratchpad byte 0x1F800207
    if (b >= 32) goto Lret;
    if (b < 25) goto Lret;
    guest_leaf(c, 0x80129E8Cu, s1);                       // FUN_80129E8C(node)  (a1 = leftover, untouched)
    c->mem_w8(s1 + 1, (uint8_t)s0);                 // node[1] = s0 (=1, delay-slot store)
  }
  goto Lret;

 La2bc:
  eng(c).spawn.despawn(s1);                         // FUN_8007A624(node)
 Lret:
  return;
}
