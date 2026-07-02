// engine/beh_actor_move_sm.cpp — PC-native per-object BEHAVIOR handler FUN_8011D988.
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
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn (FUN_8007A624)
#include "animation.h" // Animation::step (FUN_80076D68)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8011D988u;

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline uint32_t leafr3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn); return c->r[2];
}

void beh_actor_move_sm(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st < 2) {
      if (st == 0) leaf1(c, nd, 0x8011dcacu);            // FUN_8011DCAC (init)
      return;
    }
    if (st != 2) {
      if (st != 3) return;
      // STATE 3
      uint8_t f = c->mem_r8(nd + 0x1b);
      if (f & 0x40) { c->mem_w8(nd + 0x1b, (uint8_t)(f & 0xbf)); return; }
      c->mem_w32(c->mem_r32(nd + 0x10) + 0xc, 0);
      world_despawn(c, nd);                          // FUN_8007A624
      return;
    }
    // STATE 2
    leaf1(c, nd, 0x8007778cu);                            // FUN_8007778C
    {
      uint8_t n5 = c->mem_r8(nd + 5);
      uint8_t bf809 = c->mem_r8(0x800bf809u);
      uint8_t s137 = c->mem_r8(0x1f800137u);
      switch (n5) {
        case 0: case 4: case 5: case 6:
          if (bf809 == 0 && s137 == 0) leaf1(c, nd, 0x801206f4u);   // FUN_801206F4
          break;
        case 1:
          leaf1(c, nd, 0x8012175cu);                      // FUN_8012175C
          break;
        case 0xb:
          leaf1(c, nd, 0x801217f4u);                      // FUN_801217F4
          if (leafr3(c, nd, 0, 0, 0x80080750u) != 0) { c->mem_w8(nd + 4, 3); return; }  // FUN_80080750
          c->mem_w8(nd + 5, 2);
          /* fallthrough */
        case 2: case 7: case 8:
          leaf1(c, nd, 0x80120a64u);                      // FUN_80120A64
          break;
        default: break;
      }
    }
    goto Lcommon;
  }

  // STATE 1
  leaf1(c, nd, 0x8007778cu);                              // FUN_8007778C
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
        c->engine.animation.step(nd);                 // FUN_80076D68 (native)
        if (n3 != 3 && (bf809 != 0 || s137 != 0)) break;
        if ((n3 & 1) == 0) leaf1(c, nd, 0x8011dfc0u);     // FUN_8011DFC0
        else               leaf1(c, nd, 0x8011e340u);     // FUN_8011E340
        goto Ldb74;
      case 1:
        if (n3 == 3) leaf1(c, nd, 0x8011f088u);           // FUN_8011F088
        else {
          if (bf809 != 0 || s137 != 0) break;
          leaf1(c, nd, 0x8011ead0u);                      // FUN_8011EAD0
        }
        goto Ldb74;
      case 2:
        if (bf809 == 0) leaf1(c, nd, 0x8011f278u);        // FUN_8011F278
        break;
      case 3:
        if (n3 == 3 || (bf809 == 0 && s137 == 0)) {
          leaf1(c, nd, 0x8011f998u);                      // FUN_8011F998
          c->engine.animation.step(nd);               // FUN_80076D68 (native)
          c->engine.animation.step(nd);
          c->engine.animation.step(nd);
        }
        break;
      case 4:
        leaf1(c, nd, 0x8011fc78u);                        // FUN_8011FC78
       Ldb74:
        leaf1(c, nd, 0x8012185cu);                        // FUN_8012185C
        break;
      case 5:
        c->engine.animation.step(nd);                 // FUN_80076D68 (native)
        leaf1(c, nd, 0x80120c50u);                        // FUN_80120C50
        break;
      default: break;
    }
  }

 Lcommon:
  c->mem_w8(nd + 0x29, 0);
  if (c->mem_r8(nd + 1) != 0) leaf1(c, nd, 0x800518fcu);  // FUN_800518FC
}

void ov_beh_actor_move_sm(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("actor_move_smverify") ? 1 : 0;
  if (!s_v) { beh_actor_move_sm(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_actor_move_sm(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[actor_move_smverify] MISMATCH obj=%08x st=%u n5=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[actor_move_smverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_actor_move_sm_run(Core* c) { ov_beh_actor_move_sm(c); }
