// game/ai/beh_lift_platform.cpp — PC-native per-object BEHAVIOR handler FUN_8013A330.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8013A330
// (scratch/decomp/field2/8013a330.c), cross-checked against the raw disasm. A multi-segment (13-record)
// vertical mover that FOLLOWS a parent object's direction. Registers itself as the singleton
// mem32[0x800BF854]. Outer state machine on node[4]:
//   STATE 0 : INIT — mem32[0x800BF854]=node, node[0xBF]=0; if (i16)mem[0x800ED098] < 13 -> node[4]=3.
//             Else node[8]=node[9]=13, node[0x0B]=node[0x0D]=0, allocate 13 child records (FUN_8007AAE8)
//             into node[0xC0+4*i] + FUN_80051B04(rec,0xC,i+0x26); node[3]=0; if mem[0x800BFAD8]==0 &&
//             mem[0x800BF8B9]!=255 -> FUN_80118974(node[0xD0]); FUN_8013A184(node); FUN_8013989C(node);
//             then node[0]=1, node[0x82]=0xC0, node[0x29]=0, node[0x80]=0x60, node[0x84]=0x10,
//             node[0x86]=0x60, node[4]++.
//   STATE 1 : if (mem[0x800BF89C]==2 || mem[0x800E7EAA]!=1): FUN_800778E4(node, sign16(scratch[0x162]-
//             node[0x32])), s0=node[0x10] (the parent); node[1]==0 -> if parent[1]!=0 set node[1]=1 +
//             FUN_80077EBC. Else (the else-branch) s0 = the INCOMING guest s0 (c->r[16]) — faithful to
//             the recomp's uninitialized-register flow. If node[1]==0 -> tail. Then a direction machine:
//             node[0x5E] mirrors parent[0x5E] (1<->2), drives node[0x30] up/down by node[0x50]*±0x100,
//             clamps to node[0x60]/node[0x62] (toggling mem[0x800BF9EE] + node[0xBF]) and plays SFX 0x8D
//             (FUN_80074590) when parent[0xC0][0xC]&0xF00==0; a node[5] sub-machine (FUN_80139E64/
//             FUN_80139C2C/FUN_8013A008) advances/retreats; FUN_800517F8(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//   TAIL : write node[1] into each record[0x3F] (node[8] records); node[0x29]=0; FUN_80139A70(node).
//
// CONTROL FLOW + the direct node/record/global WRITES owned native; every sub-behavior CALL stays
// reachable via rec_dispatch (pure-PSX leaf). GOTCHAs vs Ghidra: mem[0x800BF8B9] `== -1` is `== 255`
// (lbu); the `unaff_s0` else-path uses the incoming guest s0 = c->r[16] (callee-saved -> preserved across
// rec_dispatch); mem[0x800ED098] is a SIGNED lh. The byte-exact A/B gate is the safety net.

#include "core.h"
#include "game_ctx.h"
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

constexpr uint32_t BEH_FN = 0x8013A330u;

// LAB_8013A60C: parent[0xC0][0xC] & 0xF00 == 0 -> SFX 0x8D.
static inline void lift_sfx(Core* c, uint32_t s0) {
  if ((c->mem_r16(c->mem_r32(s0 + 0xc0) + 0xc) & 0xf00) == 0)
    guest_leaf(c, 0x80074590u, 0x8d, 0, 0);                // FUN_80074590(0x8D,0,0)
}

}  // namespace

void beh_lift_platform(Core* c) {
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
    c->mem_w32(0x800bf854u, nd);
    c->mem_w8(nd + 0xbf, 0);
    if (c->mem_r16s(0x800ed098u) < 13) { c->mem_w8(nd + 4, 3); return; }
    c->mem_w8(nd + 8, 13);
    c->mem_w8(nd + 9, 13);
    c->mem_w8(nd + 0x0b, 0);
    c->mem_w8(nd + 0x0d, 0);
    c->r[4] = nd;                                           // a0 for the first FUN_8007AAE8
    {
      uint32_t base = nd;
      int i = 0;
      do {
        eng(c).graphicsBind.recordAlloc();                      // FUN_8007AAE8 -> rec (a0 carried)
        uint32_t rec = c->r[2];
        c->mem_w32(base + 0xc0, rec);
        eng(c).graphicsBind.installSceneRecord(rec, 0xc, (uint32_t)(i + 0x26));    // FUN_80051B04 (native)
        i++;
        base += 4;
      } while (i < (int)c->mem_r8(nd + 8));
    }
    c->mem_w8(nd + 3, 0);
    if (c->mem_r8(0x800bfad8u) == 0 && c->mem_r8(0x800bf8b9u) != 255)
      guest_leaf(c, 0x80118974u, c->mem_r32(nd + 0xd0));   // FUN_80118974(node[0xD0])
    guest_leaf(c, 0x8013a184u, nd);                        // FUN_8013A184
    guest_leaf(c, 0x8013989cu, nd);                        // FUN_8013989C
    c->mem_w8(nd + 0, 1);
    c->mem_w16(nd + 0x82, 0xc0);
    c->mem_w8(nd + 0x29, 0);
    c->mem_w16(nd + 0x80, 0x60);
    c->mem_w16(nd + 0x84, 0x10);
    c->mem_w16(nd + 0x86, 0x60);
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
    return;
  }

  // STATE 1
  uint32_t s0 = c->r[16];                                  // incoming guest s0 (else-path default)
  if (c->mem_r8(0x800bf89cu) == 2 || c->mem_r8(0x800e7eaau) != 1) {
    int32_t a1 = (int16_t)(uint16_t)((uint16_t)c->mem_r16(0x1f800162u) - (uint16_t)c->mem_r16(nd + 0x32));
    guest_leaf(c, 0x800778e4u, nd, (uint32_t)a1);          // FUN_800778E4(node, sign16(...))
    s0 = c->mem_r32(nd + 0x10);
    if (c->mem_r8(nd + 1) == 0) {
      if (c->mem_r8(s0 + 1) != 0) { c->mem_w8(nd + 1, 1); eng(c).cull.enqueueVisibleClass4(nd); }  // FUN_80077EBC — Cull::enqueueVisibleClass4
      if (c->mem_r8(nd + 1) == 0) goto L6c4;               // L508 re-check
    }
  } else {
    if (c->mem_r8(nd + 1) == 0) goto L6c4;                 // L508
  }

  // ---- node[0x5E] direction machine (uses s0) ----
  {
    uint8_t pv = c->mem_r8(s0 + 0x5e);                     // parent[0x5E]
    if (pv == 1) c->mem_w8(nd + 0x5e, 2);
    else if (pv == 2) c->mem_w8(nd + 0x5e, 1);
    else c->mem_w8(nd + 0x5e, 0);

    if (c->mem_r8(nd + 0x5e) == 1) {
      c->mem_w32(nd + 0x30, (uint32_t)((int32_t)c->mem_r32(nd + 0x30) + c->mem_r16s(nd + 0x50) * -0x100));
      if (c->mem_r16s(nd + 0x60) < c->mem_r16s(nd + 0x32)) {
        c->mem_w8(0x800bf9eeu, (uint8_t)(c->mem_r8(0x800bf9eeu) & 0xfe));
        lift_sfx(c, s0);
      } else {
        c->mem_w32(nd + 0x30, (uint32_t)(c->mem_r16s(nd + 0x60) << 16));
        c->mem_w8(0x800bf9eeu, (uint8_t)(c->mem_r8(0x800bf9eeu) | 1));
        c->mem_w8(nd + 0x5e, 0);
        c->mem_w8(nd + 0xbf, 0);
      }
    } else if (c->mem_r8(nd + 0x5e) == 2) {
      c->mem_w32(nd + 0x30, (uint32_t)((int32_t)c->mem_r32(nd + 0x30) + c->mem_r16s(nd + 0x50) * 0x100));
      if (c->mem_r16s(nd + 0x32) < c->mem_r16s(nd + 0x62)) {
        lift_sfx(c, s0);
      } else {
        c->mem_w32(nd + 0x30, (uint32_t)(c->mem_r16s(nd + 0x62) << 16));
        c->mem_w8(nd + 0x5e, 0);
        c->mem_w8(nd + 0xbf, 1);
      }
    }

    // ---- node[5] sub-machine ----
    if (c->mem_r8(nd + 5) == 0) {
      if (c->mem_r8(nd + 0x5e) == 1) {
        if (guest_leaf(c, 0x80139e64u, nd) != 0) {         // FUN_80139E64
          c->mem_w8(nd + 6, 0);
          c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));
        }
      } else if (c->mem_r8(nd + 0x5e) == 2) {
        guest_leaf(c, 0x80139c2cu, nd);                    // FUN_80139C2C
      }
    } else if (c->mem_r8(nd + 5) == 1 && guest_leaf(c, 0x8013a008u, nd, s0) != 0) {  // FUN_8013A008(node, s0)
      c->mem_w8(nd + 6, 0);
      c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) - 1));
    }
    c->r[4] = nd; eng(c).graphicsBind.renderUpdate();                             // FUN_800517F8
  }

 L6c4:
  {
    uint8_t n8 = c->mem_r8(nd + 8);
    uint8_t n1 = c->mem_r8(nd + 1);
    uint32_t base = nd;
    for (int i = 0; i < (int)n8; i++) { c->mem_w8(c->mem_r32(base + 0xc0) + 0x3f, n1); base += 4; }
    c->mem_w8(nd + 0x29, 0);
    guest_leaf(c, 0x80139a70u, nd);                        // FUN_80139A70
  }
}
