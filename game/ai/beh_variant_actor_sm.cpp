// game/ai/beh_variant_actor_sm.cpp — PC-native per-object BEHAVIOR handler FUN_8011D578.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8011D578
// (scratch/decomp/field2/8011d578.c), cross-checked against the raw disasm for the byte/half widths and
// the init gate. Outer state machine on node[4]:
//   STATE 0 : INIT, gated on FUN_800519E0(node, 0xD, mem32[0x800ECFB8], 0x8014C0BC) — bail if !=0. Else
//             node[0x7C]=0x8014DE54 (behavior table), node[0x3C]=mem32[0x800ECFBC], node[0x0B]=0x40,
//             node[0]=9, node[0x80]=0x50, node[0x82]=node[0x84]=0xA0, node[0x86]=0x120,
//             node[0x2C]=0x26DE0000, node[0x30]=0xFC040000, node[0x34]=0x16440000, node[0x2B]=node[0x29]=0,
//             node[0x58]=node[0x56]=node[0x54]=0; then per node[3]: ==0 -> node[0x56]=0xC00,node[0x7B]=7;
//             ==1 -> node[0x56]=0x400,node[0x7B]=0. FUN_80041718(node,node[0x7B],0); node[4]++.
//   STATE 1 : if node[3]==0 -> FUN_8011D108(node) then tail. If node[3]!=1 -> tail. Else (node[3]==1):
//             node[5]==0 & (scratch[0x207]!=10 || (i16)scratch[0x160]>=8000): v=FUN_8007778C(node);
//               v==0 -> if mem[0x800BF8BC]==255 node[4]=3; v!=0 & node[0x2B]==3 -> node[0x7A]=0x14,
//               FUN_80042354(1,1), FUN_80040D68(node,0x80148D2C), node[0x70]=2, node[5]++ (->LAB_d7d4).
//             node[5]==1: node[1]=1, FUN_80077E7C(node); if node[0x70]==255 node[5]-- (LAB_d7d4).
//             Then FUN_80041098(node), FUN_8004190C(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//   TAIL (LAB_d7e8) : if node[1]!=0 -> FUN_800518FC(node) + FUN_8011D82C(node); node[0x2B]=0.
//
// CONTROL FLOW + the direct node WRITES owned native; every sub-behavior CALL stays reachable via
// rec_dispatch (pure-PSX leaf). NOTE: Ghidra typed several `lbu`-compare-to-0xFF as `== -1`; the real
// compares are `== 255`. scratch[0x160] is a SIGNED 16-bit (lh). The 0x2C/0x30/0x34 stores are 32-bit
// lui-only constants (low 16 = 0). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the
// safety net.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::enqueueQueueA (FUN_80077E7C)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8011D578u;

}  // namespace

void beh_variant_actor_sm(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st >= 2) {
      if (st == 2) return;
      if (st != 3) return;
      eng(c).spawn.despawn(nd);                          // FUN_8007A624
      return;
    }
    if (st != 0) return;
    // STATE 0 (INIT)
    if (guest_leaf(c, 0x800519e0u, nd, 0xd, c->mem_r32(0x800ecfb8u), 0x8014c0bcu) != 0) return;  // FUN_800519E0
    c->mem_w32(nd + 0x7c, 0x8014de54u);
    c->mem_w32(nd + 0x3c, c->mem_r32(0x800ecfbcu));
    c->mem_w8(nd + 0x0b, 0x40);
    c->mem_w8(nd + 0, 9);
    c->mem_w16(nd + 0x80, 0x50);
    c->mem_w16(nd + 0x82, 0xa0);
    c->mem_w16(nd + 0x84, 0xa0);
    c->mem_w16(nd + 0x86, 0x120);
    c->mem_w32(nd + 0x2c, 0x26de0000u);
    c->mem_w32(nd + 0x30, 0xfc040000u);
    c->mem_w8(nd + 0x2b, 0);
    c->mem_w8(nd + 0x29, 0);
    c->mem_w16(nd + 0x58, 0);
    c->mem_w16(nd + 0x56, 0);
    c->mem_w16(nd + 0x54, 0);
    c->mem_w32(nd + 0x34, 0x16440000u);
    {
      uint8_t n3 = c->mem_r8(nd + 3);
      if (n3 == 0) { c->mem_w16(nd + 0x56, 0xc00); c->mem_w8(nd + 0x7b, 7); }
      else if (n3 == 1) { c->mem_w16(nd + 0x56, 0x400); c->mem_w8(nd + 0x7b, 0); }
    }
    guest_leaf(c, 0x80041718u, nd, c->mem_r8(nd + 0x7b), 0);   // FUN_80041718(node, node[0x7B], 0)
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
    return;
  }

  // STATE 1
  {
    uint8_t n3 = c->mem_r8(nd + 3);
    if (n3 == 0) { guest_leaf(c, 0x8011d108u, nd); goto Ltail; }   // FUN_8011D108
    if (n3 != 1) goto Ltail;

    uint8_t n5 = c->mem_r8(nd + 5);
    if (n5 == 0) {
      if (c->mem_r8(0x1f800207u) != 10 || c->mem_r16s(0x1f800160u) >= 8000) {
        if (guest_leaf(c, 0x8007778cu, nd) == 0) {           // FUN_8007778C
          if (c->mem_r8(0x800bf8bcu) == 255) c->mem_w8(nd + 4, 3);
        } else if (c->mem_r8(nd + 0x2b) == 3) {
          c->mem_w8(nd + 0x7a, 0x14);
          guest_leaf(c, 0x80042354u, 1, 1);                  // FUN_80042354(1,1)
          guest_leaf(c, 0x80040d68u, nd, 0x80148d2cu);       // FUN_80040D68(node, 0x80148D2C)
          c->mem_w8(nd + 0x70, 2);
          c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));   // LAB_d7d4
        }
      }
    } else if (n5 == 1) {
      c->mem_w8(nd + 1, 1);
      eng(c).cull.enqueueQueueA(nd);                     // FUN_80077E7C (native; return ignored)
      if (c->mem_r8(nd + 0x70) == 255)
        c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) - 1)); // LAB_d7d4
    }
    guest_leaf(c, 0x80041098u, nd);                          // FUN_80041098
    guest_leaf(c, 0x8004190cu, nd);                          // FUN_8004190C
  }

 Ltail:
  if (c->mem_r8(nd + 1) != 0) {
    guest_leaf(c, 0x800518fcu, nd);                          // FUN_800518FC
    guest_leaf(c, 0x8011d82cu, nd);                          // FUN_8011D82C
  }
  c->mem_w8(nd + 0x2b, 0);
}
