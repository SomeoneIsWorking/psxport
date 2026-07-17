// game/ai/beh_multi_record_phase_machine.cpp — PC-native per-object BEHAVIOR handler FUN_80134FD8.
//
// Overlay handler (~x778/field-frame on seaside; ~260 instr), prologue 0x80134FD8; `jr ra` at
// 0x8013540C. Disassembled from scratch/ram/field_seaside.bin. TWO-level state machine:
//   OUTER node[4]:
//     STATE 0 : INIT. mem[0x800ED098] (lh) < 10 -> node[4]=3 return. Else node[8]=node[9]=10, node[11]=1,
//               node[13]=0, node[4]++; if node[4]_orig < node[8] allocate node[8](=10) child records
//               (FUN_8007AAE8) into node[0xC0+4*i]: rec[6]=i-1, rec[0]=0, rec[4]=(i==0?0:135),
//               rec[8]=tbl@0x8014A758[i], rec[10]=rec[12]=0, FUN_80051B04(rec,12,i+52). Then node[8]=9,
//               seed node[0x60..0x6E] = 15/15295/28912/28920/-30480/-30472/40/0, node[0x32]+=1979,
//               node[0x36]-=1048.
//     STATE 1 : INNER node[5] machine (see below).
//     STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//   INNER node[5] (in OUTER state 1):
//     N5==0 : if mem[0x800BF9DD] >= 15: node[5]=2, node[0x2E]=14405, node[0x32]=-1940, node[0]=1,
//             node[0x36]=4470, and (if node[5]_orig < node[8]) rec[8]=tbl2@0x8014A76C[i] over node[8]
//             records; else node[5]=1. Then ALWAYS: node[8]++/FUN_800517F8/node[8]--, then 6x
//             FUN_801252C0(node,a1,a2) building records {[20]=node,[16]=node[0xC8/0xD8/0xC4/0xCC/0xD0]},
//             then FUN_8004CC64(node[0xD4],12).
//     N5==1 : FUN_801344AC(node); if node[6]==0 AND (mem[0x800E7EAA]-14)<5 (else node[6]!=0): node[1]=1
//             + FUN_80077EBC(node); else FUN_800779D0(node,0,-400,600). If node[1]!=0 -> common tail.
//     N5==2 : if mem[0x800E7EAA]==37 OR (mem[0x800E7EAA]-14)<6: node[1]=1 + FUN_80077EBC; else
//             FUN_8007778C. If node[1]!=0 -> FUN_801347E4(node) -> common tail.
//   COMMON TAIL : node[8]++ / FUN_800517F8(node) / node[8]--.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). a0 fidelity in the
// record loop: first FUN_8007AAE8 a0=node, FUN_80051B04 leaves a0=rec for the next iter — don't touch
// c->r[4] across the loop. The FUN_801252C0 cascade's rec[20]/rec[16] stores are mirrored BEFORE the next
// call (guest writes them in the prior call's delay slot, i.e. before the next jal). Transcribed 1:1 as a
// register machine; signed (lh/sh) preserved. The byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. NO GTE.

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

constexpr uint32_t BEH_FN = 0x80134FD8u;

// COMMON TAIL (0x801353C8): node[8]++ / FUN_800517F8(node) / node[8]--.
static inline void common_tail(Core* c, uint32_t nd) {
  c->mem_w8(nd + 8, (uint8_t)(c->mem_r8(nd + 8) + 1));
  c->r[4] = nd; eng(c).graphicsBind.renderUpdate();
  c->mem_w8(nd + 8, (uint8_t)(c->mem_r8(nd + 8) - 1));
}

}  // namespace

void beh_multi_record_phase_machine(Core* c) {
  uint32_t nd = c->r[4];                          // s1 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                // s0 = node[4] = outer state

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) { eng(c).spawn.despawn(nd); goto Lret; }
  goto Lret;

 // ================= OUTER STATE 0 (INIT) =================
 S0: {
   if (c->mem_r16s(0x800ed098u) < 10) { c->mem_w8(nd + 4, 3); goto Lret; }
   c->mem_w8(nd + 8, 10);
   c->mem_w8(nd + 9, 10);
   c->mem_w8(nd + 11, 1);
   c->mem_w8(nd + 13, 0);
   c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));    // node[4]++
   if ((int32_t)st < (int32_t)c->mem_r8(nd + 8)) {          // s0 (orig node[4]) < node[8]
     c->r[4] = nd;                                          // mirror guest a0 for first FUN_8007AAE8
     uint32_t s0 = nd;                                      // rec-store base
     uint32_t s3 = 0x8014a758u;                             // tbl base
     int i = 0;
     do {
       eng(c).graphicsBind.recordAlloc();                        // FUN_8007AAE8() -> v0 (a0 carried)
       uint32_t rec = c->r[2];
       c->mem_w32(s0 + 0xc0, rec);
       c->mem_w16(rec + 6, (uint16_t)(int16_t)(i - 1));
       c->mem_w32(rec + 0, 0);
       c->mem_w16(rec + 4, (uint16_t)(i == 0 ? 0 : 135));
       c->mem_w16(rec + 8, c->mem_r16(s3));                 // tbl[i] (lhu)
       c->mem_w16(rec + 10, 0);
       c->mem_w16(rec + 12, 0);
       s3 += 2;
       eng(c).graphicsBind.installSceneRecord(rec, 12, (uint32_t)(i + 52));         // FUN_80051B04 (native)
       i += 1;
       s0 += 4;
     } while ((int32_t)i < (int32_t)c->mem_r8(nd + 8));
   }
   c->mem_w8(nd + 8, 9);
   c->mem_w16(nd + 0x60, 15);
   c->mem_w16(nd + 0x62, 15295);
   c->mem_w16(nd + 0x64, 28912);
   c->mem_w16(nd + 0x66, 28920);
   c->mem_w16(nd + 0x68, (uint16_t)(int16_t)-30480);
   c->mem_w16(nd + 0x6a, (uint16_t)(int16_t)-30472);
   c->mem_w16(nd + 0x6c, 40);
   c->mem_w16(nd + 0x6e, 0);
   c->mem_w16(nd + 0x32, (uint16_t)(c->mem_r16(nd + 0x32) + 1979));
   c->mem_w16(nd + 0x36, (uint16_t)(c->mem_r16(nd + 0x36) - 1048));
   goto Lret;
 }

 // ================= OUTER STATE 1 (inner node[5] machine) =================
 S1: {
   uint32_t n5 = c->mem_r8(nd + 5);
   if (n5 == st) goto N5_1;                        // st == 1
   if ((int32_t)n5 < 2) { if (n5 == 0) goto N5_0; goto Lret; }
   if (n5 == 2) goto N5_2;
   goto Lret;
 }

 // -------- inner N5==0 --------
 N5_0: {
   if (c->mem_r8(0x800bf9ddu) >= 15) {
     c->mem_w8(nd + 5, 2);
     c->mem_w16(nd + 0x2e, 14405);
     c->mem_w16(nd + 0x32, (uint16_t)(int16_t)-1940);
     c->mem_w8(nd + 0, (uint8_t)st);               // node[0] = s0 = 1
     c->mem_w16(nd + 0x36, 4470);
     if ((int32_t)0 < (int32_t)c->mem_r8(nd + 8)) { // a0 = node[5]_orig = 0
       uint32_t a1t = 0x8014a76cu;                  // tbl2 base
       uint32_t a0p = nd;
       int i = 0;
       do {
         uint16_t v1 = c->mem_r16(a1t);             // tbl2[i] (lhu)
         a1t += 2;
         uint32_t rec = c->mem_r32(a0p + 0xc0);
         c->mem_w16(rec + 8, v1);
         i += 1;
         a0p += 4;
       } while ((int32_t)i < (int32_t)c->mem_r8(nd + 8));
     }
   } else {
     c->mem_w8(nd + 5, (uint8_t)st);               // node[5] = 1
   }
   // common cascade (0x80135238): node[8]++/FUN_800517F8/node[8]--, then 6x FUN_801252C0 + FUN_8004CC64
   c->mem_w8(nd + 8, (uint8_t)(c->mem_r8(nd + 8) + 1));
   c->r[4] = nd; eng(c).graphicsBind.renderUpdate();
   c->mem_w8(nd + 8, (uint8_t)(c->mem_r8(nd + 8) - 1));
   uint32_t a3;
   a3 = guest_leaf(c, 0x801252c0u, nd, 1, 0);
   c->mem_w32(a3 + 20, nd); c->mem_w32(a3 + 16, c->mem_r32(nd + 0xc8));
   a3 = guest_leaf(c, 0x801252c0u, nd, 1, 1);
   c->mem_w32(a3 + 20, nd); c->mem_w32(a3 + 16, c->mem_r32(nd + 0xd8));
   a3 = guest_leaf(c, 0x801252c0u, nd, 4, 2);
   c->mem_w32(a3 + 20, nd); c->mem_w32(a3 + 16, c->mem_r32(nd + 0xc4));
   a3 = guest_leaf(c, 0x801252c0u, nd, 4, 3);
   c->mem_w32(a3 + 20, nd); c->mem_w32(a3 + 16, c->mem_r32(nd + 0xcc));
   a3 = guest_leaf(c, 0x801252c0u, nd, 4, 4);
   c->mem_w32(a3 + 20, nd); c->mem_w32(a3 + 16, c->mem_r32(nd + 0xd0));
   guest_leaf(c, 0x8004cc64u, c->mem_r32(nd + 0xd4), 12);   // FUN_8004CC64(node[0xD4], 12)
   goto Lret;
 }

 // -------- inner N5==1 --------
 N5_1: {
   guest_leaf(c, 0x801344acu, nd);                 // FUN_801344AC(node)
   bool to354 = (c->mem_r8(nd + 6) != 0);
   if (!to354) {
     uint32_t m = c->mem_r8(0x800e7eaau);
     if ((uint32_t)(m - 14) < 5) to354 = true;
   }
   if (to354) {
     c->mem_w8(nd + 1, (uint8_t)st);               // node[1] = s0 = 1
     eng(c).cull.enqueueVisibleClass4(nd);         // FUN_80077EBC — Cull::enqueueVisibleClass4
   } else {
     guest_leaf(c, 0x800779d0u, nd, 0, (uint32_t)(int32_t)-400, 600);  // FUN_800779D0(node,0,-400,600)
   }
   if (c->mem_r8(nd + 1) == 0) goto Lret;
   common_tail(c, nd);
   goto Lret;
 }

 // -------- inner N5==2 --------
 N5_2: {
   uint32_t m = c->mem_r8(0x800e7eaau);
   bool to394 = (m == 37) || ((uint32_t)(m - 14) < 6);
   if (to394) {
     c->mem_w8(nd + 1, (uint8_t)st);               // node[1] = 1
     eng(c).cull.enqueueVisibleClass4(nd);         // FUN_80077EBC — Cull::enqueueVisibleClass4
   } else {
     guest_leaf(c, 0x8007778cu, nd);                // FUN_8007778C(node)
   }
   if (c->mem_r8(nd + 1) == 0) goto Lret;
   guest_leaf(c, 0x801347e4u, nd);                  // FUN_801347E4(node)
   common_tail(c, nd);
   goto Lret;
 }

 Lret:
  return;
}
