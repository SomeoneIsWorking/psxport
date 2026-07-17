// game/ai/beh_node3_router.cpp — PC-native per-object BEHAVIOR handler FUN_8011CBD0.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8011CBD0
// (scratch/decomp/field2/8011cbd0.c). Outer state machine on node[4]:
//   STATE 0 : INIT (only when mem[0x800BF89C] > 3) — node[0x0B]=0x40, node[0x80]=100, node[0x82]=200,
//             node[0x84]=0xC0, node[0x54]=node[0x56]=node[0x58]=0, node[0]=1, node[0x2B]=0, node[0x86]=
//             0x150, node[5]=0, node[0x3C]=mem32[0x800ECFA4] (a shared context pointer), node[4]++.
//   STATE 1 : route on node[3] — ==0 -> FUN_8011C674(node); ==1 -> FUN_8011CA04(node). If node[1]!=0 ->
//             FUN_800518FC(node). Then node[0x2B]=0 and the per-frame tail FUN_8011CD14(node).
//   STATE 2 : node[4]=3.   STATE 3 : FUN_8007A624(node).
//
// CONTROL FLOW + the direct node WRITES owned native; the sub-behavior CALLs stay reachable via
// rec_dispatch (pure-PSX leaves). Store widths from the decompile (undefined2 = 16-bit sh, byte = sb,
// the 0x3C store = 32-bit pointer). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is
// the safety net.

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

constexpr uint32_t BEH_FN = 0x8011CBD0u;

}  // namespace

void beh_node3_router(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    uint8_t n3 = c->mem_r8(nd + 3);
    if (n3 == 0) guest_leaf(c, 0x8011c674u, nd);              // FUN_8011C674
    else if (n3 == 1) guest_leaf(c, 0x8011ca04u, nd);         // FUN_8011CA04
    if (c->mem_r8(nd + 1) != 0) guest_leaf(c, 0x800518fcu, nd);   // FUN_800518FC
    c->mem_w8(nd + 0x2b, 0);
    guest_leaf(c, 0x8011cd14u, nd);                           // FUN_8011CD14
  } else if (st < 2) {
    if (st == 0 && c->mem_r8(0x800bf89cu) > 3) {
      c->mem_w8(nd + 0x0b, 0x40);
      c->mem_w16(nd + 0x80, 100);
      c->mem_w16(nd + 0x82, 200);
      c->mem_w16(nd + 0x84, 0xc0);
      c->mem_w16(nd + 0x58, 0);
      c->mem_w16(nd + 0x56, 0);
      c->mem_w16(nd + 0x54, 0);
      c->mem_w8(nd + 0, 1);
      c->mem_w8(nd + 0x2b, 0);
      c->mem_w16(nd + 0x86, 0x150);
      c->mem_w8(nd + 5, 0);
      c->mem_w32(nd + 0x3c, c->mem_r32(0x800ecfa4u));    // node[0x3C] = mem32[0x800ECFA4]
      c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
    }
  } else if (st == 2) {
    c->mem_w8(nd + 4, 3);
  } else if (st == 3) {
    eng(c).spawn.despawn(nd);                           // FUN_8007A624
  }
}
