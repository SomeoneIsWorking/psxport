// game/ai/beh_jumptable_release_trigger.cpp — PC-native per-object BEHAVIOR handler FUN_80124E74 (OVERLAY).
//
// An OVERLAY-resident per-object behavior routine (lives in the field overlay, not MAIN.EXE), installed
// at node+0x1c and called every frame by the entity walk with the object node in a0. Same SHAPE as the
// resident siblings (the FUN_739ac handler / the FUN_73cd8 handler): a state machine on the node's state byte node[4]
// (0 init / 1 active / 2 idle / 3 despawn). State 1 is a 7-way sub-machine dispatched by JUMP TABLE
// 0x80109B88 on node[3] (the object's type/sub-type, in [0,6]); state 2 is a small node[5]-gated
// special-area release path; state 3 despawns via FUN_8007A624.
//
// The node[3] jump table targets (RE'd from the field RAM dump at 0x80109B88):
//   jt[0]=0x80124f58  jt[1]=0x80124f68  jt[2]=0x80124fac  jt[3]=0x80125080
//   jt[4]=0x801250d8  jt[5]=0x80125164  jt[6]=0x80125174
//
// Ownership model (identical to the FUN_739ac handler / the FUN_73cd8 handler): CONTROL FLOW + node/global memory writes
// owned native; every sub-behavior CALL stays reachable by address via rec_dispatch (each honors its own
// override identically). NO GTE, NO render packets here. RE'd 1:1 from disas 0x80124E74..0x801252BC
// (overlay), JT 0x80109B88. It WRITES guest node state the still-recomp content reads → content-INTERFACE:
// gated byte-exact (full RAM+scratchpad A/B vs rec_super_call). Most sub-states are input/scene driven and
// only verify when a scene drives them (same caveat as the sibling orchestrators) — see Report.
//
// Globals referenced (computed from the lui/addiu pairs in the disasm):
//   0x800ECF80 (state-0 seed, lw -> node+0x3c)
//   0x800BF9DD (lbu, gate byte; <0xf => set node[0x5e])
//   0x800BF8BC (lbu, jt[6] gate; ==0xff)
//   0x800BF870 (base; +0x177 -> 0x800BF9E7, +0x178 -> 0x800BF9E8 bit-set tables in despawn tail)

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::enqueueQueueA (FUN_80077E7C)
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_set_geom
#include "inventory.h"       // class Inventory — inv(c).giveAndFlag (FUN_8004D4C4)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80124E74u;

}  // namespace

// FUN_801244E8 — release-trigger POSITION / RESPAWN sub-behavior. Called from TWO of this file's
// jump-table entries (jt[1] node[0x5e]==0 branch with mode=0; jt[4] node[0x5e]==0/2 branches with
// mode=0/1) as `jal 0x801244e8(a0=obj, a1=mode)`. RE'd verbatim from disas 0x801244E8..0x801246B0 —
// this IS a clean, self-contained function (own prologue: sw ra/s0/s1/s2/s3 + own epilogue restore;
// no register state inherited from any caller), unlike its siblings below. obj[5] is its OWN state
// byte (distinct from the caller's node[4]/node[5]): 0 = one-shot placement setup, 1 = per-frame
// respawn-adjacent-item check + reposition, >=2 = no-op (falls straight to epilogue).
//   ref = obj[0x10]              (a pointer field on the object itself — reloaded fresh, own memory)
//   state 0: obj[5]=1;
//     mode==0: obj[0x2e/32/36] = ref[0x2c]-20, ref[0x30]-200, ref[0x34]      (camera/parent offset)
//     else:    idx=(obj[0x60]-2)*6; obj[0x2e/32/36] = TBL[idx+0/2/4]        (TBL=0x801498B0, u16 stride 6)
//     obj[0xb]=0x13; obj[0x54]=0x100; FUN_80077B38(obj, 0x8014C808, 3)      (GraphicsBind::setGeom, native)
//   state 1: if (Actor::boundsCull(obj)) { identity-matrix obj+0x98; FUN_800847F0(obj+0x54,obj+0x98);
//            FUN_80084360(0x1F8000F8,obj+0x98); FUN_80077B5C(obj); }         (3 still-PSX leaves)
//     if DAT_800bf9dd==0xe: rand()&0x3f==0 -> Spawn::spawnAndInit(0x107, obj+0x2c, -10) [native];
//       reposition obj[0x2e/32/36] from ref + a random lateral offset table term; obj[5]=0, obj[6]=0,
//       obj[0x5e]=1   (RE-ARMS state 0 for the next release)
//     elif DAT_800bf9dd<0xe: FUN_80124328(obj)                              (still-PSX leaf)
static void release_position_801244e8(Core* c, uint32_t obj, uint32_t mode) {
  const uint32_t ref = c->mem_r32(obj + 0x10);
  const uint8_t st = c->mem_r8(obj + 5);
  if (st == 0) {
    c->mem_w8(obj + 5, 1);
    if (mode == 0) {
      c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(ref + 0x2c) - 20));
      c->mem_w16(obj + 0x32, (uint16_t)(c->mem_r16s(ref + 0x30) - 200));
      c->mem_w16(obj + 0x36, c->mem_r16(ref + 0x34));
    } else {
      const uint32_t TBL = 0x801498B0u;                 // 3x u16 per entry, stride 6 bytes
      int32_t idx = ((int32_t)c->mem_r16s(obj + 0x60) - 2) * 6;
      c->mem_w16(obj + 0x2e, c->mem_r16((uint32_t)(TBL + idx + 0)));
      c->mem_w16(obj + 0x32, c->mem_r16((uint32_t)(TBL + idx + 2)));
      c->mem_w16(obj + 0x36, c->mem_r16((uint32_t)(TBL + idx + 4)));
    }
    c->mem_w8(obj + 0xb, 0x13);
    c->mem_w16(obj + 0x54, 0x100);
    c->r[4] = obj; c->r[5] = 0x8014C808u; c->r[6] = 3;
    eng(c).graphicsBind.setGeom();                   // FUN_80077B38 (native)
    return;
  }
  if (st != 1) return;
  if (Actor(c, obj).boundsCull() != 0) {                // FUN_8007778C (native)
    const uint32_t xf = obj + 0x98;
    mtxOf(c).identity(xf);                                 // FUN_80051794 (native)
    c->r[4] = obj + 0x54; c->r[5] = xf;
    rec_dispatch(c, 0x800847F0u);
    c->r[4] = 0x1F8000F8u; c->r[5] = xf;
    rec_dispatch(c, 0x80084360u);
    c->r[4] = obj;
    rec_dispatch(c, 0x80077B5Cu);
  }
  const uint8_t phase = c->mem_r8(0x800BF9DDu);
  if (phase == 0xe) {
    rec_dispatch(c, 0x8009A450u);                        // FUN_8009A450 (rand, still-PSX leaf)
    if ((c->r[2] & 0x3f) == 0) {
      eng(c).spawn.spawnAndInit(0x107u, obj + 0x2c, (uint32_t)-10);   // FUN_8003116C (native)
    }
    rec_dispatch(c, 0x8009A450u);
    const uint32_t r2 = c->r[2] & 3u;
    c->mem_w16(obj + 0x2e, (uint16_t)(c->mem_r16s(ref + 0x2c) + (int32_t)(r2 - 1) * 0x28));
    c->mem_w16(obj + 0x32, c->mem_r16(ref + 0x30));
    const uint16_t last = c->mem_r16(ref + 0x34);
    c->mem_w8(obj + 0x5e, 1);
    c->mem_w8(obj + 5, 0);
    c->mem_w8(obj + 6, 0);
    c->mem_w16(obj + 0x36, last);
  } else if (phase < 0xe) {
    c->r[4] = obj;
    rec_dispatch(c, 0x80124328u);                        // still-PSX leaf
  }
}

void beh_jumptable_release_trigger(Core* c) {
  const uint32_t obj = c->r[4];                 // 80124E7C  move s2, a0  (s2 = obj node ptr)
  uint8_t st = c->mem_r8(obj + 4);              // 80124E90  lbu v1, 4(s2)  (state byte)

  // 80124E98  beq v1, 1 -> state 1 (0x80124f28)
  // 80124E9C  slti v0, v1, 2 ; 80124EA0 beqz -> v1>=2 (0x80124eb8)
  if (st != 1) {
    if (st >= 2) {
      // ---- 80124EB8: v1 >= 2 ----
      // 80124EB8 beq v1,2 -> state 2 (0x801251f0) ; 80124EC0 beq v1,3 -> state 3 (0x8012529c)
      if (st == 2) {
        goto state2;
      }
      if (st == 3) {
        // ---- STATE 3 (0x8012529c): despawn ----
        eng(c).spawn.despawn(obj);   // 8012529C jal 0x8007a624 (a0=s2)
        return;                                        // 801252A4 epilogue
      }
      return;                                          // 80124EC8 j 0x801252a4 (epilogue, no-op)
    }
    if (st != 0) return;                               // 80124EA8 beqz v1 -> state 0; else dead-fallthrough epilogue

    // ---- STATE 0 (0x80124ed0): one-shot init ----
    c->mem_w8 (obj + 4, 1);                                  // 80124ED0 sb s0(=1), 4(s2)  -> state 1
    int32_t g = (int32_t)c->mem_r32(0x800ECF80u);           // 80124ED4 lw v1, -0x3080(800f)  (signed lw)
    c->mem_w8 (obj + 0xb, 0x12);                             // 80124EDC sb 0x12, 0xb(s2)
    c->mem_w16(obj + 0x80, 0x1e);                            // 80124EE4 sh 0x1e, 0x80(s2)
    c->mem_w16(obj + 0x82, 0x28);                            // 80124EEC sh 0x28, 0x82(s2)
    c->mem_w16(obj + 0x84, 0x10);                            // 80124EF4 sh 0x10, 0x84(s2)
    c->mem_w16(obj + 0x5c, 0);                               // 80124EFC sh zero, 0x5c(s2)
    c->mem_w8 (obj + 0x46, 0);                               // 80124F00 sb zero, 0x46(s2)
    c->mem_w8 (obj + 0x47, 0);                               // 80124F04 sb zero, 0x47(s2)
    c->mem_w16(obj + 0x54, 0);                               // 80124F08 sh zero, 0x54(s2)
    c->mem_w16(obj + 0x56, 0);                               // 80124F0C sh zero, 0x56(s2)
    c->mem_w16(obj + 0x58, 0);                               // 80124F10 sh zero, 0x58(s2)
    c->mem_w16(obj + 0x86, 0x20);                            // 80124F14 sh 0x20, 0x86(s2)  (v0=0x20 from 80124EF8)
    c->mem_w8 (obj + 0x29, 0);                               // 80124F18 sb zero, 0x29(s2)
    c->mem_w16(obj + 0x6c, 0);                               // 80124F1C sh zero, 0x6c(s2)
    c->mem_w32(obj + 0x3c, (uint32_t)g);                     // 80124F24 sw v1, 0x3c(s2)  (delay slot of j epilogue)
    return;                                                  // 80124F20 j 0x801252a4 (epilogue)
  }

  // ============================================================================================
  // STATE 1 (0x80124f28): node[3] jump-table sub-machine
  // ============================================================================================
  {
    uint8_t n3 = c->mem_r8(obj + 3);                  // 80124F28 lbu v1, 3(s2)
    if (n3 >= 7) goto epi_done;                       // 80124F30 sltiu v0,v1,7 ; 80124F34 beqz -> 0x801251e8
    // 80124F40 sll v1,2 ; addu base ; lw v0 ; jr v0  -> JT 0x80109B88
    switch (n3) {

    case 0:   // jt[0] = 0x80124f58
      c->r[4] = obj; rec_dispatch(c, 0x801241BCu);    // 80124F58 jal 0x801241bc (a0=s2)
      c->mem_w8(obj + 0x29, 0);                        // 80124F64 sb zero, 0x29(s2)  (delay slot)
      goto epilogue;                                   // 80124F60 j 0x801252a4

    case 1: { // jt[1] = 0x80124f68
      // 80124F6C lbu v0, 0x800bf9dd ; 80124F74 sltiu v0,v0,0xf ; 80124F78 bnez -> skip set
      if (c->mem_r8(0x800BF9DDu) >= 0xf) {
        c->mem_w8(obj + 0x5e, 1);                      // 80124F80 sb 1, 0x5e(s2)  (v0=1 from 80124F7C)
      }
      uint8_t v = c->mem_r8(obj + 0x5e);              // 80124F84 lbu v1, 0x5e(s2)
      // 80124F8C beqz -> 0x80125130 (delay move a0,s2) ; 80124F94 bne v1,1 -> 0x801251e8
      if (v == 0) {
        // 80125130: jal 0x801244e8(a0=s2, a1=0)
        release_position_801244e8(c, obj, 0);           // FUN_801244E8 (native)
        c->mem_w8(obj + 0x29, 0);                       // 80125140 sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 8012513C j 0x801252a4
      }
      if (v != 1) goto epi_done;                        // 80124F94 bne v1,1 -> 0x801251e8
      // v==1: jal 0x801246b4(a0=s2)
      c->r[4] = obj; rec_dispatch(c, 0x801246B4u);     // 80124F9C jal 0x801246b4
      c->mem_w8(obj + 0x29, 0);                          // 80124FA8 sb zero, 0x29(s2)  (delay slot)
      goto epilogue;                                     // 80124FA4 j 0x801252a4
    }

    case 2: { // jt[2] = 0x80124fac : node[6] sub-switch
      uint8_t n6 = c->mem_r8(obj + 6);                 // 80124FAC lbu v1, 6(s2)
      uint32_t s0 = c->mem_r32(obj + 0x10);            // 80124FB0 lw s0, 0x10(s2)  (a pointer)
      // s3 = s2 = obj (80124FB8 move s3,s2)
      // 80124FB4 beqz -> 0x80124fd0 (n6==0) ; 80124FC0 beq v1,1 -> 0x8012501c ; else j epi
      bool fall_into_1 = false;
      if (n6 == 0) {
        // ---- 80124fd0: sub-case 0 (then falls through into sub-case 1) ----
        c->r[4] = obj;                                  // 80124FD0 move a0,s2
        c->r[5] = 0x8015C808u;                          // 80124FD4 lui a1,0x8015 ; 80124FD8 addiu a1,-0x37f8
        c->r[6] = 4;                                    // 80124FDC addiu a2,zero,4
        eng(c).graphicsBind.setGeom();                   // 80124FE4 jal 0x80077b38
        c->mem_w8 (obj + 6, (uint8_t)c->r[2]);          // 80124FE8 sb v0, 6(s2)  (delay slot; v0=ret)
        c->mem_w8 (obj + 0xb, 0x10);                    // 80124FF0 sb 0x10, 0xb(s2)
        c->mem_w16(obj + 0x5a, 0xa00);                  // 80124FF8 sh 0xa00, 0x5a(s2)
        c->mem_w8 (obj + 8, 0xfc);                      // 80125000 sb 0xfc, 8(s2)
        c->mem_w16(obj + 0x88, 0x18);                   // 80125008 sh 0x18, 0x88(s2)
        c->mem_w8 (obj + 0x47, 0);                      // 80125010 sb zero, 0x47(s2)
        c->mem_w16(obj + 0x8a, 0x50);                   // 80125014 sh 0x50, 0x8a(s2)
        c->mem_w16(obj + 0x8c, 0);                      // 80125018 sh zero, 0x8c(s2)
        fall_into_1 = true;
      } else if (n6 != 1) {
        c->mem_w8(obj + 0x29, 0);                       // 80124FCC sb zero, 0x29(s2)  (delay slot of j epi)
        goto epilogue;                                  // 80124FC8 j 0x801252a4
      }
      // ---- 8012501c: sub-case 1 (n6==1, or fell through from 0) ----
      (void)fall_into_1;
      if (c->mem_r8(s0 + 0x3f) == 0) goto epi_done;     // 8012501C lbu v0,0x3f(s0) ; 80125024 beqz -> 0x801251e8
      // FUN_80077E7C → Cull::enqueueQueueA (native). Returns 0 on cap-hit, new count on push.
      c->mem_w8(obj + 1, (uint8_t)eng(c).cull.enqueueQueueA(obj));  // sb v0, 1(s3) (was rec_dispatch)
      // jal 0x80051d90(a0=s0, a1=s3+0x88, a2=0x1f8000c0)
      c->r[4] = s0;                                     // 80125038 move a0,s0
      c->r[5] = obj + 0x88;                             // 8012503C addiu a1,s3,0x88
      c->r[6] = 0x1F8000C0u;                            // 80125040 lui s1,0x1f80 ; 80125044 addiu s0,s1,0xc0 ; move a2,s0
      rec_dispatch(c, 0x80051D90u);                     // 80125048 jal 0x80051d90
      // read scratchpad results written by 0x80051d90 into node fields
      uint16_t r0 = c->mem_r16(0x1F8000C0u);           // 80125050 lhu v0, 0xc0(s1)  (=0x1f8000c0)
      c->mem_w16(obj + 0x2e, r0);                       // 80125058 sh v0, 0x2e(s3)
      uint16_t r2 = c->mem_r16(0x1F8000C0u + 2);        // 8012505C lhu v0, 2(s0)  (s0=0x1f8000c0)
      c->mem_w16(obj + 0x32, r2);                       // 80125064 sh v0, 0x32(s3)
      uint16_t r4 = c->mem_r16(0x1F8000C0u + 4);        // 80125068 lhu v0, 4(s0)
      // jal 0x80077b5c(a0=s3=obj) ; sh v0,0x36(a0)
      c->r[4] = obj;                                    // 8012506C move a0,s3
      c->mem_w16(obj + 0x36, r4);                       // 80125074 sh v0, 0x36(a0)  (delay slot)
      rec_dispatch(c, 0x80077B5Cu);                     // 80125070 jal 0x80077b5c
      c->mem_w8(obj + 0x29, 0);                          // 8012507C sb zero, 0x29(s2)  (delay slot)
      goto epilogue;                                     // 80125078 j 0x801252a4
    }

    case 3: { // jt[3] = 0x80125080 : node[6] sub-switch
      uint8_t n6 = c->mem_r8(obj + 6);                 // 80125080 lbu v1, 6(s2)
      // 80125088 beqz -> 0x801250a0 (n6==0) ; 80125090 beq v1,1 -> 0x801250c0 ; else j epi
      if (n6 == 0) {
        // ---- 801250a0: sub-case 0 ----
        c->mem_w8(obj + 6, 1);                          // 801250A0 sb v0(=1), 6(s2)
        c->r[4] = obj;                                  // 801250A4 move a0,s2
        c->r[5] = 0x8015C808u;                          // 801250A8 lui/addiu a1
        c->r[6] = 4;                                    // 801250B4 addiu a2,zero,4 (delay slot)
        eng(c).graphicsBind.setGeom();                   // 801250B0 jal 0x80077b38
        c->mem_w8(obj + 0x29, 0);                       // 801250BC sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 801250B8 j 0x801252a4
      }
      if (n6 != 1) {
        c->mem_w8(obj + 0x29, 0);                       // 8012509C sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 80125098 j 0x801252a4
      }
      // ---- 801250c0: sub-case 1 ----
      c->r[4] = obj; rec_dispatch(c, 0x8004DAECu);      // 801250C0 jal 0x8004daec (a0=s2)
      if (c->r[2] == 0) goto epi_done;                  // 801250C8 beqz v0 -> 0x801251e8 (delay addiu v0,3)
      c->mem_w8(obj + 4, 3);                            // 801250D4 sb v0(=3), 4(s2)  -> state 3 next frame
      goto epi_done;                                    // 801250D0 j 0x801251e8
    }

    case 4: { // jt[4] = 0x801250d8 : node[5]-gated sub-machine
      // 801250DC lbu v0,0x800bf9dd ; 801250E4 sltiu v0,v0,0xf ; 801250E8 bnez -> skip ; (delay v0=2)
      if (c->mem_r8(0x800BF9DDu) >= 0xf) {
        c->mem_w8(obj + 0x5e, 2);                       // 801250F0 sb 2, 0x5e(s2)
      }
      uint8_t v = c->mem_r8(obj + 0x5e);               // 801250F4 lbu v1, 0x5e(s2)
      // 801250FC beq v1,1 -> 0x80125144 ; 80125100 slti v0,v1,2 ; 80125104 beqz -> 0x8012511c (v1>=2)
      if (v == 1) {
        // ---- 80125144 ----
        c->r[4] = obj; rec_dispatch(c, 0x801249D4u);    // 80125144 jal 0x801249d4 (a0=s2)
        c->mem_w8(obj + 0x29, 0);                       // 80125150 sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 8012514C j 0x801252a4
      }
      if (v < 2) {
        // v==0 -> 80125134 (80125108 beqz v1 -> 0x80125134, delay move a0,s2)
        release_position_801244e8(c, obj, 0);            // FUN_801244E8 (native)
        c->mem_w8(obj + 0x29, 0);                       // 80125140 sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 8012513C j 0x801252a4
      }
      // v>=2 -> 8012511c
      // 8012511C addiu v0,zero,2 ; 80125120 beq v1,2 -> 0x80125154 ; else j epi
      if (v == 2) {
        // ---- 80125154 ----
        release_position_801244e8(c, obj, 1);            // FUN_801244E8 (native)
        c->mem_w8(obj + 0x29, 0);                       // 80125160 sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 8012515C j 0x801252a4
      }
      c->mem_w8(obj + 0x29, 0);                          // 8012512C sb zero, 0x29(s2)  (delay slot of j epi @ 80125128)
      goto epilogue;                                     // 80125128 j 0x801252a4
    }

    case 5: // jt[5] = 0x80125164
      c->r[4] = obj; rec_dispatch(c, 0x80124C6Cu);      // 80125164 jal 0x80124c6c (a0=s2)
      c->mem_w8(obj + 0x29, 0);                          // 80125170 sb zero, 0x29(s2)  (delay slot)
      goto epilogue;                                     // 8012516C j 0x801252a4

    case 6: { // jt[6] = 0x80125174 : node[6] sub-switch
      uint8_t n6 = c->mem_r8(obj + 6);                 // 80125174 lbu v1, 6(s2)
      // 8012517C beqz -> 0x80125194 (n6==0) ; 80125184 beq v1,1 -> 0x801251bc ; else j epi
      if (n6 == 0) {
        // ---- 80125194 ----
        // 80125198 lbu v1,0x800bf8bc ; 801251A0 bne v1,0xff -> 0x801251e8 ; (delay v0=1)
        if (c->mem_r8(0x800BF8BCu) != 0xff) goto epi_done;  // 801251A0 bne -> 0x801251e8
        uint16_t v2e = c->mem_r16(obj + 0x2e);          // 801251A8 lhu v1, 0x2e(s2)
        c->mem_w8 (obj + 6, 1);                          // 801251AC sb 1, 6(s2)
        c->mem_w8 (obj + 0, 1);                          // 801251B0 sb 1, 0(s2)
        c->mem_w16(obj + 0x7e, v2e);                     // 801251B8 sh v1, 0x7e(s2)  (delay slot)
        goto epi_done;                                   // 801251B4 j 0x801251e8
      }
      if (n6 != 1) {
        c->mem_w8(obj + 0x29, 0);                       // 80125190 sb zero, 0x29(s2)  (delay slot)
        goto epilogue;                                  // 8012518C j 0x801252a4
      }
      // ---- 801251bc: sub-case 1 ----
      if (Actor(c, obj).boundsCull() == 0) goto epi_done;   // 801251BC jal 0x8007778c — Actor::boundsCull (native)
      c->r[4] = obj; rec_dispatch(c, 0x80123E9Cu);      // 801251CC jal 0x80123e9c (a0=s2)
      uint16_t a = c->mem_r16(obj + 0x7e);             // 801251D4 lhu v0, 0x7e(s2)
      uint16_t b = c->mem_r16(obj + 0x64);             // 801251D8 lhu v1, 0x64(s2)
      c->mem_w16(obj + 0x2e, (uint16_t)(a + b));        // 801251E4 sh v0, 0x2e(s2)  (v0 = a+b @ 801251E0)
      goto epi_done;                                    // (falls to 0x801251e8 region)
    }

    } // switch(n3)
  }

epi_done:
  // ---- 0x801251e8: shared tail before epilogue ----
  c->mem_w8(obj + 0x29, 0);                            // 801251EC sb zero, 0x29(s2)  (delay slot of j epi)
  // 801251E8 j 0x801252a4
  return;                                              // 801252A4 epilogue

epilogue:
  // direct j 0x801252a4 paths (node[0x29] already written in the respective delay slot above)
  return;

state2:
  // ============================================================================================
  // STATE 2 (0x801251f0): cull, then node[5]-range special-area release
  // ============================================================================================
  {
    Actor(c, obj).boundsCull();                       // 801251F0 jal 0x8007778c (result IGNORED) — Actor::boundsCull (native)
    int8_t sub = c->mem_r8s(obj + 5);          // 801251F8 lbu v1, 5(s2)
    // 80125200 bltz v1 -> epilogue ; 80125204 slti v0,v1,2 ; 80125208 bnez -> epilogue (v1<2)
    // 8012520C slti v0,v1,4 ; 80125210 beqz -> epilogue (v1>=4)   => active only when 2<=v1<4
    if (sub < 0 || sub < 2 || sub >= 4) return;        // 801252A4 epilogue
    // 2 <= node[5] < 4:
    inv(c).giveAndFlag(0x77, 1);                  // 8012521C jal 0x8004d4c4 [native]
    c->r[4] = obj; rec_dispatch(c, 0x8004B0D8u);        // 80125224 jal 0x8004b0d8 (a0=s2)
    eng(c).sfx.trigger(0x11, 0, 0);                  // 80125234 jal 0x80074590 (native)
    uint8_t n3 = c->mem_r8(obj + 3);                   // 8012523C lbu v1, 3(s2)
    c->mem_w8(obj + 4, 3);                              // 80125248 sb v0(=3), 4(s2)  -> state 3  (delay slot)
    // 80125244 bnez v1 -> 0x8012526c
    if (n3 == 0) {
      // ---- 8012524c: node[3]==0 -> set bit (1<<node[0x60]) in 0x800BF9E7 ----
      int16_t sh = c->mem_r16s(obj + 0x60);    // 80125254 lh v1, 0x60(s2)  (signed lh; shift amount)
      uint8_t cur = c->mem_r8(0x800BF9E7u);            // 80125258 lbu a0, 0x177(v0=0x800bf870)
      uint32_t bit = (uint32_t)1u << (sh & 31);         // 8012525C sllv v1, s0(=1), v1
      c->mem_w8(0x800BF9E7u, (uint8_t)(cur | bit));     // 80125260 or ; 80125268 sb a0, 0x177(v0)
      return;                                           // 80125264 j 0x801252a4 epilogue
    }
    // 8012526C beq v1,1 -> 0x8012527c ; 80125270 (delay v0=5) ; 80125274 bne v1,5 -> epilogue
    if (n3 == 1 || n3 == 5) {
      // ---- 8012527c: node[3]==1 or 5 -> set bit (1<<node[0x60]) in 0x800BF9E8 ----
      int16_t sh = c->mem_r16s(obj + 0x60);    // 80125284 lh v1, 0x60(s2)
      uint8_t cur = c->mem_r8(0x800BF9E8u);            // 80125288 lbu a0, 0x178(v0=0x800bf870)
      uint32_t bit = (uint32_t)1u << (sh & 31);         // 8012528C sllv v1, s0(=1), v1
      c->mem_w8(0x800BF9E8u, (uint8_t)(cur | bit));     // 80125290 or ; 80125298 sb a0, 0x178(v0)
      return;                                           // 80125294 j 0x801252a4 epilogue
    }
    return;                                             // 80125274 bne -> epilogue (other n3)
  }
}
