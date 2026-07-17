// game/ai/beh_typed_anim_spawn.cpp — PC-native per-object BEHAVIOR handler FUN_8012DA04.
//
// Overlay handler (~x2331/field-frame on seaside; ~200 instr), prologue 0x8012DA04; `jr ra` at
// 0x8012DE2C. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4] (v1):
//   STATE 0 : INIT — node[3]<8 dispatches in-overlay jump table jt0 @0x80109DAC; sets node[4]=1.
//       jt0 case 0/1/2/6/7 -> reset block (node[11]=16, node[8]=240, node[13]/0x5A/0x5C/0x47/0x60=0);
//       case 3/4 -> FUN_80051B70(node,12,79); on 0 set node[0xC0][8]=+182(n3==3)/-182(n3==4) +
//                   FUN_800517F8(node); case 5 -> FUN_80051B70(node,12,7); on 0 FUN_800517F8(node).
//   STATE 1 : node[3]<8 dispatches jt1 @0x80109DCC (the per-node[3] animation/spawn sub-states); the
//       cases drive node[5] sub-state, call FUN_80077B38 (model-attach, a1=0x8014C808) on entry, then
//       FUN_8004BD64 (5-arg, arg5 = &node[0x60] passed on the stack) and copy node[1] from node[0x10][1];
//       several gate on area bytes 0x800BF9DD/0x800BF9B5 to advance node[4]=3 / run the FUN_8004D4C4+
//       FUN_8004B0D8 tail. jt1 case 1 -> FUN_8018C574; case 3/4/5 -> FUN_8007778C.
//   STATE 2 : if node[3]==2 run the FUN_8004D4C4(29,1)+FUN_8004B0D8 tail, node[4]=3.
//   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). Guest a0..a3 + the ONE
// stack argument (sp+16) are set exactly as the guest does — so we mirror the guest prologue (sp -= 40)
// so FUN_8004BD64 reads arg5 from the same frame slot the recomp does (that slot lives in the gate's
// excluded stack window, so it never shows in the RAM diff, but the leaf must read the right value).
// Both jump tables READ live from resident overlay RAM. Transcribed 1:1 as a register machine; the
// byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_set_geom
#include "inventory.h"       // class Inventory — inv(c).giveAndFlag (FUN_8004D4C4)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012DA04u;
constexpr uint32_t A1_MODEL = 0x8014C808u;   // FUN_80077B38 arg (lui 0x8015 + addiu -14328)

}  // namespace

void beh_typed_anim_spawn(Core* c) {
  uint32_t s0 = c->r[4];                            // s0 = a0 (node)
  uint32_t entry_sp = c->r[29];
  c->r[29] = entry_sp - 40;                         // mirror the guest prologue frame

  uint32_t v1 = c->mem_r8(s0 + 4);                  // node[4] = outer state
  uint8_t n3;
  uint32_t s1;

  if (v1 == 1) goto S1;
  if ((int32_t)v1 < 2) { if (v1 == 0) goto S0; goto Lret; }
  if (v1 == 2) goto S2;
  if (v1 == 3) goto S3;
  goto Lret;                                        // v1 >= 4

 // ---------------- STATE 0 ----------------
 S0:
  n3 = c->mem_r8(s0 + 3);
  if (!(n3 < 8)) goto Lret;
  c->mem_w8(s0 + 4, 1);                             // node[4] = 1 (delay-slot store)
  switch (n3) {
    case 0: case 1: case 2: case 6: case 7:         // jt0 -> 0x8012da90
      c->mem_w8 (s0 + 11, 16);                      // node[11] = 16
      c->mem_w8 (s0 + 13, 0);                       // node[13] = 0
      c->mem_w16(s0 + 0x5C, 0);
      c->mem_w16(s0 + 0x5A, 0);
      c->mem_w8 (s0 + 0x47, 0);
      c->mem_w16(s0 + 0x60, 0);
      c->mem_w8 (s0 + 8, 240);                      // node[8] = 240 (delay-slot store)
      goto Lret;
    case 3: case 4: {                               // jt0 -> 0x8012dab8
      (c->r[4]=s0, c->r[5]=12, c->r[6]=79, eng(c).graphicsBind.recordInit());            // FUN_80051B70(node, 12, 79)
      if (c->r[2] != 0) goto Lret;
      uint32_t rec = c->mem_r32(s0 + 0xC0);         // node[0xC0]
      c->mem_w16(rec + 8, (uint16_t)(n3 == 3 ? 182 : (uint16_t)(int16_t)-182));
      c->r[4] = s0; eng(c).graphicsBind.renderUpdate();                    // FUN_800517F8(node)
      goto Lret;
    }
    case 5:                                         // jt0 -> 0x8012db08
      (c->r[4]=s0, c->r[5]=12, c->r[6]=7, eng(c).graphicsBind.recordInit());             // FUN_80051B70(node, 12, 7)
      if (c->r[2] != 0) goto Lret;
      c->r[4] = s0; eng(c).graphicsBind.renderUpdate();                    // FUN_800517F8(node)
      goto Lret;
  }
  goto Lret;

 // ---------------- STATE 1 ----------------
 S1:
  n3 = c->mem_r8(s0 + 3);
  if (!(n3 < 8)) goto Lret;
  switch (n3) {
    case 0: {                                       // jt1 -> 0x8012db60
      s1 = c->mem_r8(s0 + 5);                       // node[5]
      if (s1 == 0) {                                // 0x8012db88
        if (c->mem_r8(0x800BF9DDu) != 9) goto Lret;
        c->mem_w8(s0 + 5, 1);                       // node[5] = 1
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u)); // node[0x3C] = *(0x800ECF80) (delay slot)
        (c->r[4]=s0, c->r[5]=A1_MODEL, c->r[6]=15, eng(c).graphicsBind.setGeom());    // FUN_80077B38(node, 0x8014C808, 15)
        c->mem_w16(s0 + 0x60, 64);
        c->mem_w16(s0 + 0x62, 0);
        c->mem_w16(s0 + 0x64, (uint16_t)(int16_t)-32);
        goto Lret;
      }
      if (s1 == 1) {                                // 0x8012dbd8
        c->r[4] = s0; c->r[5] = 2;
        c->r[6] = c->mem_r32(0x800E7F5Cu); c->r[7] = c->mem_r32(0x800E7F50u);
        c->mem_w32(c->r[29] + 16, s0 + 0x60);
        eng(c).graphicsBind.posCompose();                      // FUN_8004BD64(node,2,*0x800E7F5C,*0x800E7F50,&node[0x60])
        c->mem_w8(s0 + 1, (uint8_t)s1);             // node[1] = s1 (=1)
        if (c->mem_r8(0x800BF9DDu) != 11) goto Lret;
        c->mem_w8(s0 + 4, 3);                       // node[4] = 3
        goto Lret;
      }
      goto Lret;                                    // s1 >= 2
    }
    case 1:                                         // jt1 -> 0x8012dc10
      guest_leaf(c, 0x8018C574u, s0);                // FUN_8018C574(node)
      goto Lret;
    case 2: {                                       // jt1 -> 0x8012dc20
      s1 = c->mem_r8(s0 + 5);                       // node[5]
      uint32_t rec = c->mem_r32(s0 + 0x10);         // node[0x10]
      if (s1 == 0) {                                // 0x8012dc40
        c->mem_w8(s0 + 5, 1);                       // node[5] = 1
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u));
        (c->r[4]=s0, c->r[5]=A1_MODEL, c->r[6]=2, eng(c).graphicsBind.setGeom());     // FUN_80077B38(node, 0x8014C808, 2)
        c->mem_w16(s0 + 0x60, 32);
        c->mem_w16(s0 + 0x62, 16);
        c->mem_w16(s0 + 0x64, 0);
      } else if (s1 != 1) goto Lret;                // s1 >= 2
      // fall-through (s1==0 and s1==1) -> 0x8012dc7c
      c->r[4]=s0; c->r[5]=1; c->r[6]=c->mem_r32(rec + 0xDC); c->r[7]=0;
      c->mem_w32(c->r[29] + 16, s0 + 0x60); eng(c).graphicsBind.posCompose();   // FUN_8004BD64 — native
      c->mem_w8(s0 + 1, c->mem_r8(rec + 1));        // node[1] = node[0x10][1]
      goto Lret;
    }
    case 3: case 4: case 5:                         // jt1 -> 0x8012dca0
      guest_leaf(c, 0x8007778Cu, s0);                // FUN_8007778C(node)
      goto Lret;
    case 6: {                                       // jt1 -> 0x8012dcb0
      s1 = c->mem_r8(s0 + 5);
      uint32_t rec = c->mem_r32(s0 + 0x10);
      if (s1 == 0) {                                // 0x8012dcd0
        c->mem_w8(s0 + 5, 1);
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u));
        (c->r[4]=s0, c->r[5]=A1_MODEL, c->r[6]=17, eng(c).graphicsBind.setGeom());    // FUN_80077B38(node, 0x8014C808, 17)
        c->mem_w16(s0 + 0x60, (uint16_t)(int16_t)-200);
        c->mem_w16(s0 + 0x62, 32);
        c->mem_w16(s0 + 0x64, 0);
      } else if (s1 != 1) goto Lret;
      // fall-through -> 0x8012dd0c
      c->r[4]=s0; c->r[5]=2; c->r[6]=0; c->r[7]=c->mem_r32(rec + 0xD0);
      c->mem_w32(c->r[29] + 16, s0 + 0x60); eng(c).graphicsBind.posCompose();   // FUN_8004BD64 — native
      c->mem_w8(s0 + 1, c->mem_r8(rec + 1));        // node[1] = node[0x10][1]
      if (c->mem_r8(0x800BF9B5u) != 5) goto Lret;
      goto Ltail14;                                 // 0x8012ddfc with a0=14
    }
    case 7: {                                       // jt1 -> 0x8012dd4c
      s1 = c->mem_r8(s0 + 5);
      uint32_t rec = c->mem_r32(s0 + 0x10);
      if (s1 == 0) {                                // 0x8012dd6c
        c->mem_w8(s0 + 5, 1);
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u));
        (c->r[4]=s0, c->r[5]=A1_MODEL, c->r[6]=14, eng(c).graphicsBind.setGeom());    // FUN_80077B38(node, 0x8014C808, 14)
        c->mem_w16(s0 + 0x60, (uint16_t)(int16_t)-200);
        c->mem_w16(s0 + 0x62, 48);
        c->mem_w16(s0 + 0x64, 0);
      } else if (s1 != 1) goto Lret;
      // fall-through -> 0x8012dda8
      c->r[4]=s0; c->r[5]=2; c->r[6]=0; c->r[7]=c->mem_r32(rec + 0xD0);
      c->mem_w32(c->r[29] + 16, s0 + 0x60); eng(c).graphicsBind.posCompose();   // FUN_8004BD64 — native
      c->mem_w8(s0 + 1, c->mem_r8(rec + 1));        // node[1] = node[0x10][1]
      if (c->mem_r8(0x800BF9B5u) != 6) goto Lret;
      goto Ltail35;                                 // 0x8012ddfc with a0=35
    }
  }
  goto Lret;

 // ---------------- STATE 2 ----------------
 S2:
  if (c->mem_r8(s0 + 3) != 2) goto Lret;            // node[3] != node[4](=2)
  // a0 = 29 -> shared tail 0x8012ddfc
  inv(c).giveAndFlag(29, 1);                  // FUN_8004D4C4(29, 1) [native]
  goto Ltail_after;

 Ltail14:
  inv(c).giveAndFlag(14, 1);                  // FUN_8004D4C4(14, 1) [native]
  goto Ltail_after;
 Ltail35:
  inv(c).giveAndFlag(35, 1);                  // FUN_8004D4C4(35, 1) [native]
  goto Ltail_after;
 Ltail_after:
  guest_leaf(c, 0x8004B0D8u, s0);                    // FUN_8004B0D8(node)
  c->mem_w8(s0 + 4, 3);                             // node[4] = 3
  goto Lret;

 // ---------------- STATE 3 ----------------
 S3:
  eng(c).spawn.despawn(s0);                 // FUN_8007A624(node)
 Lret:
  c->r[29] = entry_sp;                              // restore caller sp (mirror epilogue)
  return;
}
