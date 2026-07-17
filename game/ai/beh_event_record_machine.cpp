// game/ai/beh_event_record_machine.cpp — PC-native per-object BEHAVIOR handler FUN_80136954.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_80136954
// (scratch/decomp/field2/80136954.c), cross-checked against the raw disasm. A 4-record object that fires
// an event/cutscene via a node[5] jump-table machine (table @0x80119EC4). Outer state on node[4]:
//   STATE 0 : INIT — if (i16)mem[0x800ED098] < 4 -> node[4]=3. Else node[8]=node[9]=4, node[0x0B]=3,
//             node[0x0D]=0, node[4]++; allocate 4 child records (FUN_8007AAE8) into node[0xC0+4*i] seeded
//             from the 5-short table @0x8014A7E4 (rec[6]=t0, rec[0]=t1, rec[2]=t2, rec[4]=t3, rec[8]=
//             rec[0xC]=0, FUN_80051B04(rec,0xC,(i16)t4)); seed node[0x60..0x6E]/node[0xBA]/node[0x50]/
//             node[0x2B].
//   STATE 1 : v=FUN_8007778C(node); if v==0 return. switch(node[5]): 0-> mem[0x800BF8B9]==255 gate
//             (FUN_80051B04 / a big seed + node[5]=4 / node[5]=1) else node[5]=2; 2-> mem[0x800BF9DD]==12
//             -> node[5]++; 3-> FUN_8013681C; 4-> per node[6] sub-phase fire an event (set DAT_800E7E84/85
//             + FUN_80042354 / FUN_80042310 / FUN_8006FF10 spawn + FUN_800440E4). Then FUN_800517F8(node);
//             node[0x2B]=0.
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//
// CONTROL FLOW + the direct node/record/global WRITES owned native; every sub-behavior CALL stays
// reachable via rec_dispatch (pure-PSX leaf). GOTCHAs vs Ghidra: `mem[0x800BF8B9] == -1` is `== 255`
// (lbu); `DAT_800bf80c._2_1_` is mem8(0x800BF80E); mem[0x800ED098] is a signed lh; the case-4 FUN_800440E4
// + node[6]++ is reached ONLY by the n6==1 / n6==2-success fallthrough (the other sub-cases break first).
// The byte-exact A/B gate is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "object/actor.h"     // Actor::boundsCull (FUN_8007778C — thin wrapper native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80136954u;

static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}
static inline uint32_t leafr2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_event_record_machine(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st >= 2) {
      if (st == 2) return;
      if (st != 3) return;
      eng(c).spawn.despawn(nd);                           // FUN_8007A624
      return;
    }
    if (st != 0) return;
    // STATE 0 (INIT)
    if (c->mem_r16s(0x800ed098u) < 4) { c->mem_w8(nd + 4, 3); return; }
    c->mem_w8(nd + 8, 4);
    c->mem_w8(nd + 9, 4);
    c->mem_w8(nd + 0x0b, 3);
    c->mem_w8(nd + 0x0d, 0);
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
    c->r[4] = nd;                                           // a0 for the first FUN_8007AAE8
    {
      uint32_t base = nd, tbl = 0x8014a7e4u;
      int i = 0;
      do {
        i++;
        eng(c).graphicsBind.recordAlloc();                      // FUN_8007AAE8 -> rec (a0 carried)
        uint32_t rec = c->r[2];
        c->mem_w32(base + 0xc0, rec);
        c->mem_w16(rec + 6, c->mem_r16(tbl + 0));
        c->mem_w16(rec + 0, c->mem_r16(tbl + 2));
        c->mem_w16(rec + 2, c->mem_r16(tbl + 4));
        c->mem_w16(rec + 4, c->mem_r16(tbl + 6));
        c->mem_w32(rec + 8, 0);
        c->mem_w32(rec + 0xc, 0);
        int16_t t4 = c->mem_r16s(tbl + 8);
        tbl += 10;
        eng(c).graphicsBind.installSceneRecord(rec, 0xc, (uint32_t)(int32_t)t4);   // FUN_80051B04 (native)
        base += 4;
      } while (i < (int)c->mem_r8(nd + 8));
    }
    c->mem_w16(nd + 0x60, 0xf);
    c->mem_w16(nd + 0x62, 0x3bbf);
    c->mem_w16(nd + 0x64, 0x70f0);
    c->mem_w16(nd + 0x66, 0x70f8);
    c->mem_w16(nd + 0x68, 0x88f0);
    c->mem_w16(nd + 0x6a, 0x88f8);
    c->mem_w16(nd + 0x6c, 0x28);
    c->mem_w16(nd + 0x6e, 0);
    c->mem_w16(nd + 0xba, 0xf0);
    c->mem_w16(nd + 0x50, 0);
    c->mem_w8(nd + 0x2b, 0);
    return;
  }

  // STATE 1
  if (Actor(c, nd).boundsCull() == 0) return;              // FUN_8007778C — Actor::boundsCull
  switch (c->mem_r8(nd + 5)) {
    case 0:
      if (c->mem_r8(0x800bf8b9u) == 255) {
        if (c->mem_r8(0x800bf937u) == 0) {
          guest_leaf(c, 0x80051b04u, c->mem_r32(nd + 0xc0), 0xc, 0xb);
        } else if ((c->mem_r8(0x800bfa4bu) & 1) == 0) {
          c->mem_w8(nd + 0, 1);
          c->mem_w16(nd + 0x80, 0x15e);
          c->mem_w16(nd + 0x82, 700);
          c->mem_w16(nd + 0x84, 200);
          c->mem_w16(nd + 0x86, 400);
          guest_leaf(c, 0x80051b04u, c->mem_r32(nd + 0xc0), 0xc, 0xb);
          c->mem_w8(nd + 5, 4);
          break;
        }
        c->mem_w8(nd + 5, 1);
      } else {
        c->mem_w8(nd + 5, 2);
      }
      break;
    case 2:
      if (c->mem_r8(0x800bf9ddu) == 12) c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));
      break;
    case 3:
      guest_leaf(c, 0x8013681cu, nd);                           // FUN_8013681C
      break;
    case 4: {
      uint8_t n6 = c->mem_r8(nd + 6);
      uint32_t uVar5 = 0;
      if (n6 == 1) {
        if (c->mem_r8(0x800bf80eu) == 0) break;
        uVar5 = 0xcf;
      } else if (n6 < 2) {                                  // n6 == 0
        if (c->mem_r8(nd + 0x2b) == 3) {
          c->mem_w8(0x800e7e84u, 4);
          c->mem_w8(0x800e7e85u, 0x21);
          c->mem_w8(0x800e7e86u, 0);
          c->mem_w8(0x800e7ee1u, 0);
          c->mem_w8(nd + 0, 2);
          c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
          guest_leaf(c, 0x80042354u, 1, 1);                     // FUN_80042354(1,1)
        }
        break;
      } else if (n6 != 2) {                                 // n6 >= 3
        if (n6 == 3 && (c->mem_r8(0x800bf822u) & 1) == 0) {
          guest_leaf(c, 0x80042310u);                           // FUN_80042310()
          c->mem_w8(nd + 6, 0);
          c->mem_w8(nd + 5, 1);
        }
        break;
      } else {                                             // n6 == 2
        if (c->mem_r8(0x800bf80eu) == 0) break;
        uint32_t iv = leafr2(c, nd, 0x24, 0x8006ff10u);    // FUN_8006FF10(node,0x24)
        uint32_t uVar3 = c->mem_r32(0x800e7eb4u);
        uVar5 = c->mem_r32(0x800e7eb0u);
        c->mem_w32(0x800bf844u, iv);
        if (iv == 0) break;
        c->mem_w32(iv + 0x2c, c->mem_r32(0x800e7eacu));
        c->mem_w32(iv + 0x30, uVar5);
        c->mem_w32(iv + 0x34, uVar3);
        guest_leaf(c, 0x80051b04u, c->mem_r32(nd + 0xc0), 0xc, 0x4e);
        uVar5 = 2;
        c->mem_w8(0x800bfa4bu, (uint8_t)(c->mem_r8(0x800bfa4bu) | 1));
      }
      // reached only by n6==1 (uVar5=0xCF) / n6==2-success (uVar5=2):
      guest_leaf(c, 0x800440e4u, 0x800e7e80u, uVar5, 4);         // FUN_800440E4(0x800E7E80, uVar5, 4)
      c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
      break;
    }
    default: break;
  }
  c->r[4] = nd; eng(c).graphicsBind.renderUpdate();                               // FUN_800517F8
  c->mem_w8(nd + 0x2b, 0);
}
