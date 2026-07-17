// game/ai/beh_actor_move_sm.cpp — PC-native per-object BEHAVIOR handler FUN_8011D988.
//
// Overlay handler (~x776/field-frame on seaside). Ported from the Ghidra decompile of FUN_8011D988
// (scratch/decomp/field2/8011d988.c). A rich movement actor with two per-frame phases (outer node[4]==1
// vs ==2), each a switch on the movement sub-state node[5]. Outer state machine on node[4]:
//   STATE 0 : FUN_8011DCAC(node) (one-shot init).
//   STATE 2 : FUN_8007778C(node); then switch(node[5]): {0,4,5,6}-> (mem[0x800BF809]==0 &&
//             scratch[0x137]==0) ? FUN_801206F4; 1-> FUN_8012175C; 0xB-> FUN_801217F4 + if
//             FUN_80080750(node,0,0)!=0 -> node[4]=3 return, else node[5]=2 FALLTHROUGH; {2,7,8}->
//             FUN_80120A64. Then the common tail.
//   STATE 1 : FUN_8007778C(node); if node[0x2B]!=0 -> node[0x2B]-- + common tail; else switch(node[5]):
//             0-> FUN_80076D68 + (node[3]!=3 && (bf809||s137)) break, else node[3]&1 ? FUN_8011E340 :
//             FUN_8011DFC0 -> FUN_8012185C; 1-> node[3]==3 ? FUN_8011F088 : (bf809||s137 ? break :
//             FUN_8011EAD0) -> FUN_8012185C; 2-> bf809==0 ? FUN_8011F278; 3-> (node[3]==3 || (!bf809 &&
//             !s137)) ? FUN_8011F998 + 3x FUN_80076D68; 4-> FUN_8011FC78 -> FUN_8012185C; 5->
//             FUN_80076D68 + FUN_80120C50. Then the common tail.
//   STATE 3 : if node[0x1B]&0x40 -> clear that bit, return; else mem32[node[0x10]+0xC]=0 + FUN_8007A624.
//   COMMON TAIL : node[0x29]=0; if node[1]!=0 -> FUN_800518FC(node).
//
// CONTROL FLOW + the direct node WRITES owned native; every sub-behavior CALL stays reachable via
// rec_dispatch (pure-PSX leaf). Switch fallthroughs (state-2 case 0xB -> 2/7/8) and the cross-case
// `goto LAB_8011db74` (state-1 cases 0/1 -> case 4's FUN_8012185C) are preserved exactly. The byte-exact
// A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "render/render.h"       // Core::mRender (NodeXform)
#include "object/actor.h"     // Actor::boundsCull (FUN_8007778C — thin wrapper native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "animation.h" // Animation::step (FUN_80076D68)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8011D988u;

static inline uint32_t leafr3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_actor_move_sm(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st < 2) {
      if (st == 0) guest_leaf(c, 0x8011dcacu, nd);            // FUN_8011DCAC (init)
      return;
    }
    if (st != 2) {
      if (st != 3) return;
      // STATE 3
      uint8_t f = c->mem_r8(nd + 0x1b);
      if (f & 0x40) { c->mem_w8(nd + 0x1b, (uint8_t)(f & 0xbf)); return; }
      c->mem_w32(c->mem_r32(nd + 0x10) + 0xc, 0);
      eng(c).spawn.despawn(nd);                          // FUN_8007A624
      return;
    }
    // STATE 2
    Actor(c, nd).boundsCull();                             // FUN_8007778C — Actor::boundsCull (thin wrapper native)
    {
      uint8_t n5 = c->mem_r8(nd + 5);
      uint8_t bf809 = c->mem_r8(0x800bf809u);
      uint8_t s137 = c->mem_r8(0x1f800137u);
      switch (n5) {
        case 0: case 4: case 5: case 6:
          if (bf809 == 0 && s137 == 0) guest_leaf(c, 0x801206f4u, nd);   // FUN_801206F4
          break;
        case 1:
          guest_leaf(c, 0x8012175cu, nd);                      // FUN_8012175C
          break;
        case 0xb:
          guest_leaf(c, 0x801217f4u, nd);                      // FUN_801217F4
          if (leafr3(c, nd, 0, 0, 0x80080750u) != 0) { c->mem_w8(nd + 4, 3); return; }  // FUN_80080750
          c->mem_w8(nd + 5, 2);
          /* fallthrough */
        case 2: case 7: case 8:
          guest_leaf(c, 0x80120a64u, nd);                      // FUN_80120A64
          break;
        default: break;
      }
    }
    goto Lcommon;
  }

  // STATE 1
  Actor(c, nd).boundsCull();                              // FUN_8007778C — Actor::boundsCull
  if (c->mem_r8(nd + 0x2b) != 0) {
    c->mem_w8(nd + 0x2b, (uint8_t)(c->mem_r8(nd + 0x2b) - 1));
    goto Lcommon;
  }
  {
    uint8_t n5 = c->mem_r8(nd + 5);
    uint8_t bf809 = c->mem_r8(0x800bf809u);
    uint8_t s137 = c->mem_r8(0x1f800137u);
    uint8_t n3 = c->mem_r8(nd + 3);
    switch (n5) {
      case 0:
        eng(c).animation.step(nd);                 // FUN_80076D68 (native)
        if (n3 != 3 && (bf809 != 0 || s137 != 0)) break;
        if ((n3 & 1) == 0) guest_leaf(c, 0x8011dfc0u, nd);     // FUN_8011DFC0
        else               guest_leaf(c, 0x8011e340u, nd);     // FUN_8011E340
        goto Ldb74;
      case 1:
        if (n3 == 3) guest_leaf(c, 0x8011f088u, nd);           // FUN_8011F088
        else {
          if (bf809 != 0 || s137 != 0) break;
          guest_leaf(c, 0x8011ead0u, nd);                      // FUN_8011EAD0
        }
        goto Ldb74;
      case 2:
        if (bf809 == 0) guest_leaf(c, 0x8011f278u, nd);        // FUN_8011F278
        break;
      case 3:
        if (n3 == 3 || (bf809 == 0 && s137 == 0)) {
          guest_leaf(c, 0x8011f998u, nd);                      // FUN_8011F998
          eng(c).animation.step(nd);               // FUN_80076D68 (native)
          eng(c).animation.step(nd);
          eng(c).animation.step(nd);
        }
        break;
      case 4:
        guest_leaf(c, 0x8011fc78u, nd);                        // FUN_8011FC78
       Ldb74:
        guest_leaf(c, 0x8012185cu, nd);                        // FUN_8012185C
        break;
      case 5:
        eng(c).animation.step(nd);                 // FUN_80076D68 (native)
        guest_leaf(c, 0x80120c50u, nd);                        // FUN_80120C50
        break;
      default: break;
    }
  }

 Lcommon:
  c->mem_w8(nd + 0x29, 0);
  if (c->mem_r8(nd + 1) != 0) rend(c)->mNodeXform.buildWithOffset(nd);   // FUN_800518FC (native)
}
