// game/ai/beh_pure_inner_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80136D9C.
//
// Overlay handler (~x2334/field-frame on seaside; ~90 instr), prologue 0x80136D9C; `jr ra` at
// 0x80136F00. Disassembled from scratch/ram/field_seaside.bin. A pure CONTROL-FLOW dispatcher: it
// owns NO node writes of its own — every effect happens inside the sub-behavior leaves it routes to.
// Two-level dispatch (outer node[4], inner node[5]):
//   STATE 0 : FUN_80136F08(node).        STATE 2 : nothing.       STATE 3 : FUN_8007A624(node).  >=4: nothing.
//   STATE 1 : unless (byte 0x800BF89C != 2 && node[3]==2 && byte 0x800E7EAA==1), call FUN_8007778C(node);
//             then inner node[5]: 0 -> node[3]==3 ? FUN_8018CDC4 : FUN_80138A64; 1 -> byte 0x800BF809==0 ?
//             FUN_80137198 : (skip); 2 -> FUN_8018CA1C; then if node[1]!=0 -> FUN_801389C8(node).
//
// Ownership model: CONTROL FLOW owned native; all 8 sub-behavior CALLs stay reachable via rec_dispatch
// (pure-PSX leaf, a0=node). NO GTE/render. Transcribed 1:1 as a register machine (locals = guest regs,
// goto labels = guest addresses). The handler writes no RAM, so the A/B gate (full RAM+scratchpad vs
// rec_super_call) only confirms the routing chose the same leaves in the same order.

#include "core.h"
#include "game_ctx.h"
#include "object/actor.h"     // Actor::boundsCull (FUN_8007778C — thin wrapper native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80136D9Cu;

}  // namespace

void beh_pure_inner_dispatch(Core* c) {
  uint32_t obj = c->r[4];                         // s0 = a0 (node)

  uint8_t st = c->mem_r8(obj + 4);                // node[4] = outer state
  if (st == 1) goto Le00;
  if (st < 2) { if (st == 0) goto Ldf0; goto Lret; }   // st<2 -> only st==0 reachable
  if (st == 2) goto Lret;
  if (st == 3) goto Lef0;
  goto Lret;                                       // st >= 4 default

 Lef0:  eng(c).spawn.despawn(obj); goto Lret;      // STATE 3
 Ldf0:  guest_leaf(c, 0x80136F08u, obj); goto Lret;   // STATE 0

 Le00:                                             // STATE 1
  // call FUN_8007778C unless (0x800BF89C != 2 && node[3]==2 && 0x800E7EAA==1)
  if (c->mem_r8(0x800BF89Cu) != 2 && c->mem_r8(obj + 3) == 2 && c->mem_r8(0x800E7EAAu) == 1)
    goto Le3c;
  Actor(c, obj).boundsCull();          // FUN_8007778C — Actor::boundsCull
 Le3c:
  {
    uint8_t n5 = c->mem_r8(obj + 5);              // inner state
    if (n5 == 1) goto Lea4;
    if (n5 >= 2) { if (n5 == 2) goto Lec8; goto Led0; }
    // n5 == 0
    if (c->mem_r8(obj + 3) == 3) guest_leaf(c, 0x8018CDC4u, obj);
    else                         guest_leaf(c, 0x80138A64u, obj);
    goto Led0;
  }
 Lea4:                                             // n5 == 1
  if (c->mem_r8(0x800BF809u) == 0) guest_leaf(c, 0x80137198u, obj);
  goto Led0;
 Lec8:                                             // n5 == 2
  guest_leaf(c, 0x8018CA1Cu, obj);
  // fall into Led0
 Led0:
  if (c->mem_r8(obj + 1) != 0) guest_leaf(c, 0x801389C8u, obj);
 Lret:
  return;
}
