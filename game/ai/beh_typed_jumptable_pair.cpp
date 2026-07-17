// game/ai/beh_typed_jumptable_pair.cpp — PC-native per-object BEHAVIOR handler FUN_80138FC8 (OVERLAY).
//
// An OVERLAY-resident per-object behavior routine (lives in the field overlay, not MAIN.EXE), installed
// at node+0x1c and called every frame by the entity walk with the object node in a0. Same SHAPE as the
// resident/overlay siblings (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler / the FUN_80124e74 handler): a state
// machine on the node's state byte node[4] (0 init / 1 active / 2 idle / 3 despawn).
//
//   state 0 -> per-type init (FUN_80051b70 cull-record init with a2 = TA[node3]; if it returns !=0 the
//              init is busy and we bail). On success: state++, seed a block of node fields, set bit 0x80 of
//              node[0x28]; then if TB[node3] != 0 set node[0x2a] = TB[node3] and seed node+0x60/0x62/0x64/
//              0x5f/0x80..0x86 from the per-type struct table TC (12-byte stride, indexed by node[3]); then
//              fall into a node[3]-keyed jump table (JT0 @0x80109F44, node[3] in [0,5]) that pokes a couple
//              of extra fields. If TB[node3]==0 we skip straight to the JT0 block.
//   state 1 -> a global-gate (0x800BF816/0x800BF817) front-end choosing FUN_80077ebc / FUN_8007703c /
//              the plain cull FUN_8007778c, then a node[3]-keyed jump table (JT1 @0x80109F5C, node[3] in
//              [0,5]) of per-type sub-machines, then a common tail: if node[1]!=0 FUN_800517f8, then
//              node[0x29]=0, node[0x2b]=0.
//   state 2 -> return (idle)
//   state 3 -> FUN_8007a624 (despawn)
//
// Jump tables (decoded from the field RAM dump):
//   JT0 @0x80109F44 (state-0, node[3]): {8013924c, 8013924c, 80139200, 8013924c, 80139258, 80139260}
//   JT1 @0x80109F5C (state-1, node[3]): {8013931c, 801393f8, 80139414, 8013947c, 80139578, 80139580}
//
// Per-type data tables it READS from guest RAM (NOT hardcoded — read live with mem_r*, exactly as the
// recomp body does, so they stay correct if the overlay is reloaded with different data):
//   TA @0x8014A9A8  lbu[node3]            (a2 to FUN_80051b70)
//   TB @0x8014A9B0  lbu[node3]            (-> node[0x2a]; 0 => skip the TC seed + JT0 block)
//   TC @0x8014A9B8  struct[node3], 12-byte stride: lh@0, lhu@2, lh@4, lbu@6, lhu@8, lhu@0xa
//   TD @0x8014AA38  struct[(s1n3*2 + node3)*8]: lhu@0/@2/@4  (s1 = node[0x10] ptr) — read in JT1 case 2
//
// Ownership model (identical to the siblings): CONTROL FLOW + node/global memory writes owned native;
// every sub-behavior CALL stays a reachable PSX leaf via rec_dispatch (NO recursion into them). NO GTE,
// NO render packets here. RE'd 1:1 from disas 0x80138FC8..0x801395BC (epilogue jr ra @0x801395B8; the next
// function has its own prologue at 0x801395C0). It WRITES guest node state the still-recomp content reads
// -> content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B vs rec_super_call). The idle/active field
// path is exercised by the gate; the input/scene-driven sub-states are faithfully transcribed and verify
// when a scene drives them (same caveat as the sibling orchestrators) — see Report.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::enqueueByClass (FUN_8007703C)
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_record_init
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80138FC8u;

constexpr uint32_t TA = 0x8014A9A8u;   // lui 0x8015, addiu -0x5658
constexpr uint32_t TB = 0x8014A9B0u;   // lui 0x8015, addiu -0x5650
constexpr uint32_t TC = 0x8014A9B8u;   // lui 0x8015, addiu -0x5648  (12-byte stride)
constexpr uint32_t TD = 0x8014AA38u;   // lui 0x8015, addiu -0x55c8  ( 8-byte stride, JT1 case 2)

}  // namespace

void beh_typed_jumptable_pair(Core* c) {
  const uint32_t obj = c->r[4];                       // 80138FD0  move s0, a0
  uint8_t st = c->mem_r8(obj + 4);                    // 80138FDC  lbu v1, 4(s0)

  // ---- state dispatch [0x80138FE4..0x80139018] ----
  // 80138FE4 beq v1,1 -> state1 (0x80139274) ; 80138FE8 slti v0,v1,2 ; 80138FEC beqz -> v1>=2 (0x80139004)
  if (st != 1) {
    if (st >= 2) {                                    // ---- 0x80139004 ----
      // 80139008 beq v1,2 -> epilogue (0x801395ac) ; 80139010 beq v1,3 -> despawn (0x801395a4)
      if (st == 2) return;                            // idle
      if (st == 3) {                                  // 0x801395A4: despawn
        eng(c).spawn.despawn(obj);  // 801395A4 jal 0x8007a624 (a0=s0)
      }
      return;                                         // 80139018 j 0x801395ac (epilogue, no-op)
    }
    if (st != 0) return;                              // 80138FF4 beqz v1 -> state0; else j 0x801395ac

    // ============================================================================================
    // STATE 0 (0x80139020): cull-record init + per-type field seed
    // ============================================================================================
    // 80139020 lui v0,0x8015 ; 80139028 addiu v0,-0x5658 -> TA ; addu v1,node3,TA ; lbu a2,(v1)
    uint8_t a2 = c->mem_r8(TA + c->mem_r8(obj + 3));  // 80139030 lbu a2, (TA + node3)
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = a2;       // 80139038 a1=0xc ; a2 (delay) ; a0=s0
    eng(c).graphicsBind.recordInit();                     // 80139034 jal 0x80051b70
    if (c->r[2] != 0) return;                         // 8013903C bnez v0 -> 0x801395ac (init busy)

    uint16_t v56 = c->mem_r16(obj + 0x56);            // 80139048 lhu v1, 0x56(s0)
    c->mem_w8 (obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));  // 80139044/50 lbu v0,4 ; +1 ; sb -> state 1
    uint8_t v28 = c->mem_r8(obj + 0x28);              // 80139054 lbu v0, 0x28(s0)
    c->mem_w8 (obj + 0xbf, 1);                        // 80139058 sb s1(=1), 0xbf(s0)
    c->mem_w16(obj + 0x50, 0);                        // 8013905C sh zero, 0x50(s0)
    c->mem_w8 (obj + 0x29, 0);                        // 80139060 sb zero, 0x29(s0)
    c->mem_w16(obj + 0x5a, v56);                      // 80139064 sh v1(=old node[0x56]), 0x5a(s0)
    // 80139068 lbu v1, 3(s0)  (node3, used by the TB lookup below)
    c->mem_w8 (obj + 0x28, (uint8_t)(v28 | 0x80));    // 8013906C/70 ori v0,0x80 ; sb v0, 0x28(s0)

    // 80139074 lui v0,0x8015 ; 80139078 addiu v0,-0x5650 -> TB ; addu v1,node3,TB ; lbu v1,(v1)
    uint8_t tb = c->mem_r8(TB + c->mem_r8(obj + 3));  // 80139080 lbu v1, (TB + node3)
    if (tb != 0) {                                    // 80139088 beqz v1 -> 0x801391d0 (skip seed)
      c->mem_w8(obj + 0x2a, tb);                      // 80139090 sb v1, 0x2a(s0)

      // ---- TC struct seed (12-byte stride, base 0x8014A9B8), indexed by node[3] ----
      // node[0x60]: if TC.lh@0 != 0 -> node[0x60] = TC.lhu@0 - node[0x2e]; else node[0x60] = TC.lhu@0
      uint8_t  n3   = c->mem_r8(obj + 3);             // 80139098 lbu a0, 3(s0)
      uint32_t e    = TC + (uint32_t)n3 * 12;         // 801390A0..AC sll/addu/sll/addu -> TC + n3*12
      int16_t  e0s  = c->mem_r16s(e + 0);     // 801390B0 lh   v1, (v0)
      uint16_t e0u  = c->mem_r16(e + 0);              // 801390B4 lhu  a0, (v0)
      if (e0s != 0) {                                 // 801390B8 beqz v1 -> 0x801390d4
        uint16_t n2e = c->mem_r16(obj + 0x2e);        // 801390C0 lhu  v0, 0x2e(s0)
        c->mem_w16(obj + 0x60, (uint16_t)(e0u - n2e));// 801390C8/D0 subu ; sh v0, 0x60(s0)
      } else {
        c->mem_w16(obj + 0x60, e0u);                  // 801390D4 sh a0, 0x60(s0)
      }
      // node[0x64]: TC struct re-derived (n3 reloaded), uses .lh@4 / .lhu@4
      n3  = c->mem_r8(obj + 3);                       // 801390DC lbu a0, 3(s0)
      e   = TC + (uint32_t)n3 * 12;                   // 801390E0..F0
      int16_t  e4s = c->mem_r16s(e + 4);      // 801390F4 lh   v1, 4(v0)
      uint16_t e4u = c->mem_r16(e + 4);               // 801390F8 lhu  a0, 4(v0)
      if (e4s != 0) {                                 // 801390FC beqz v1 -> 0x80139118
        uint16_t n36 = c->mem_r16(obj + 0x36);        // 80139104 lhu  v0, 0x36(s0)
        c->mem_w16(obj + 0x64, (uint16_t)(e4u - n36));// 8013910C/14 subu ; sh v0, 0x64(s0)
      } else {
        c->mem_w16(obj + 0x64, e4u);                  // 80139118 sh a0, 0x64(s0)
      }
      // a1 = TC base (0x8014A9B8) for the remaining reads; node3 in v1 and a0 both = node[3]
      n3 = c->mem_r8(obj + 3);                        // 80139124/28 lbu v1,3 ; lbu a0,3
      uint32_t e2 = TC + (uint32_t)n3 * 12;           // 8013912C..38 (v1 path)
      c->mem_w16(obj + 0x62, c->mem_r16(e2 + 2));     // 8013913C/44 lhu v0,2(v0) ; sh v0, 0x62(s0)
      c->mem_w8 (obj + 0x5f, c->mem_r8 (e2 + 6));     // 80139158/60 lbu v0,6(v0) ; sb v0, 0x5f(s0)

      uint16_t e8 = c->mem_r16(e2 + 8);               // 80139174 lhu v0, 8(v0)
      c->mem_w16(obj + 0x82, e8);                     // 8013917C sh v0, 0x82(s0)
      // node[0x80] = (sign-extend e8) >> 1  with round-toward-zero (sra after +sign>>31)
      int16_t  e8s = (int16_t)e8;                     // 80139180/84 sll 16 ; sra 16
      int32_t  half80 = ((int32_t)e8s + ((uint32_t)(int32_t)e8s >> 31)) >> 1;  // 80139188..90 srl 31 ; addu ; sra 1
      // a0 path uses node3 again for node[0x86]/node[0x84]
      uint32_t e3 = TC + (uint32_t)c->mem_r8(obj + 3) * 12;  // 80139194..A0 (a0 path; a0 still = node3)
      c->mem_w8 (obj + 0, 1);                         // 801391AC/B0 addiu v1,zero,1 ; sb v1, (s0)
      uint16_t ea = c->mem_r16(e3 + 0xa);             // 801391A8 lhu v0, 0xa(v0)
      c->mem_w16(obj + 0x80, (uint16_t)(int16_t)half80);  // 801391A4 sh v1(=half80), 0x80(s0)
      c->mem_w16(obj + 0x86, ea);                     // 801391B4 sh v0, 0x86(s0)
      int16_t  eas = (int16_t)ea;                     // 801391B8/BC sll 16 ; sra 16
      int32_t  half84 = ((int32_t)eas + ((uint32_t)(int32_t)eas >> 31)) >> 1;  // 801391C0..C8
      c->mem_w16(obj + 0x84, (uint16_t)(int16_t)half84);  // 801391CC sh v1(=half84), 0x84(s0)
    }

    // ---- 0x801391D0: node[3]-keyed jump table JT0 (node[3] in [0,5]) ----
    uint8_t n3 = c->mem_r8(obj + 3);                  // 801391D0 lbu v1, 3(s0)
    if (n3 >= 6) return;                              // 801391DC beqz (sltiu v1<6) -> 0x801395ac
    switch (n3) {                                     // JT0 @0x80109F44
      case 0: case 1: case 3:                         // jt0[0]/[1]/[3] = 0x8013924c
        c->mem_w8(obj + 0x46, 1);                     // 8013924C/54 addiu v0,zero,1 ; sb v0, 0x46(s0)
        return;                                       // 80139250 j 0x801395ac
      case 2: {                                       // jt0[2] = 0x80139200
        c->mem_w8(obj + 0x46, 1);                     // 80139200/0C addiu a1,zero,1 ; sb a1, 0x46(s0)
        // v1 base = 0x800c0000 - 0x7f8 = 0x800BF808
        uint8_t g0e = c->mem_r8(0x800BF816u);         // 80139210 lbu v0, 0xe(0x800bf808)
        if (g0e == 0) return;                         // 80139218 beqz v0 -> 0x801395ac
        uint8_t g0f = c->mem_r8(0x800BF817u);         // 80139220 lbu v1, 0xf(0x800bf808)
        if (g0f != 2) return;                         // 80139228 bne v1,2 -> 0x801395ac
        c->mem_w8(obj + 4, 1);                        // 80139234 sb a1(=1), 4(s0)
        c->mem_w8(obj + 5, 0);                        // 80139238 sb zero, 5(s0)
        // FUN_80072EFC: obj[+0x56] = obj[+0x5A] - obj[+0x50]  (inlined; disas 0x80072EFC..0x80072F10)
        c->mem_w16(obj + 0x56, (uint16_t)(c->mem_r16(obj + 0x5A) - c->mem_r16(obj + 0x50)));
        c->mem_w8(obj + 6, (uint8_t)g0f);             // 80139240 sb v1(=g0f), 6(s0)  (delay slot)
        return;                                       // 80139244 j 0x801395ac
      }
      case 4:                                         // jt0[4] = 0x80139258
        c->mem_w8(obj + 0x46, 0);                     // 80139258/5C sb zero, 0x46(s0)
        return;                                       // 80139258 j 0x801395ac
      case 5:                                         // jt0[5] = 0x80139260
        c->mem_w16(obj + 0x56, (uint16_t)(c->mem_r16(obj + 0x56) - 0x600));  // 80139260/68/70 lhu ; -0x600 ; sh
        return;                                       // 8013926C j 0x801395ac
    }
    return;
  }

  // ============================================================================================
  // STATE 1 (0x80139274): global-gate front-end, then node[3] jump table JT1, then common tail
  // ============================================================================================
  // gate base v1 = 0x800c0000 - 0x7f8 = 0x800BF808
  if (c->mem_r8(0x800BF816u) != 0) {                  // 8013927C lbu v0, 0xe(base) ; 80139284 beqz -> 0x801392b0
    // 0x80139274 branch taken: gate byte 0x800BF816 != 0
    if (c->mem_r8(0x800BF817u) == c->mem_r8(obj + 3)) {  // 8013928C lbu v1,0xf(base) ; 80139290 lbu v0,3(s0) ; 80139298 bne -> 0x801392ec
      eng(c).cull.enqueueVisibleClass4(obj);       // FUN_80077EBC — Cull::enqueueVisibleClass4  (801392A0 jal 0x80077ebc)
    }
    // else j 0x801392ec
  } else {                                            // ---- 0x801392B0: gate byte == 0 ----
    // GOTCHA: branch POLARITY. 801392C8 `sltiu v0,v0,0x1d` => v0=1 when (0x1F800207 < 0x1d);
    // 801392CC `bnez v0 -> 0x801392e4` => when (val < 0x1d) we TAKE the branch to the cull
    // FUN_8007778c. FUN_8007703c is reached only on the FALL-THROUGH, i.e. when (val >= 0x1d).
    // (An earlier version had this inverted, which mis-dispatched the leaf for obj node[3]==2 and
    //  diverged at node+1 / scratch 0x148.)
    if (c->mem_r8(obj + 3) == 2 && c->mem_r8(0x1F800207u) >= 0x1d) {  // 801392B0 bne v1,2 ; 801392C0..CC val>=0x1d
      eng(c).cull.enqueueByClass(obj);             // 801392D4 jal 0x8007703c — Cull::enqueueByClass (native)
    } else {
      Actor(c, obj).boundsCull();                     // 801392E4 jal 0x8007778c — Actor::boundsCull (native)
    }
  }

  // ---- 0x801392EC: node[3] jump table JT1 (node[3] in [0,5]) ----
  uint8_t n3 = c->mem_r8(obj + 3);                    // 801392EC lbu v1, 3(s0)
  if (n3 < 6) {                                       // 801392F8 beqz (sltiu v1<6) -> 0x80139580 (tail)
    switch (n3) {                                     // JT1 @0x80109F5C

    case 0: {                                         // jt1[0] = 0x8013931c : node[5] sub-machine
      // a1 = 0x800e0000 + 0x7e80 = 0x800E7E80 (s1-base used by the sub-cases)
      const uint32_t S1 = 0x800E7E80u;               // 8013931C lui v0,0x800e ; 8013932C addiu a1,v0,0x7e80
      uint8_t s5 = c->mem_r8(obj + 5);               // 80139320 lbu a0, 5(s0)
      // 80139328 beq a0,1 -> 0x80139398 ; 80139330 slti v0,a0,2 ; 80139334 beqz -> a0>=2 (0x8013934c)
      if (s5 == 1) {
        // ---- 0x80139398: node[5] == 1 ----
        c->mem_w8(obj + 5, 2);                        // 80139398/9C addiu v0,zero,2 ; sb v0, 5(s0)
        uint8_t n5f = c->mem_r8(obj + 0x5f);         // 801393A0 lbu v0, 0x5f(s0)
        c->mem_w8(obj + 0x29, (uint8_t)s5);          // 801393A8 sb a0(=node[5]==1), 0x29(s0)
        // 0x800c0000 - 0x7e1 = 0x800BF81F ; value = (node[5] - node[0x5f]) << 4
        uint8_t v = (uint8_t)(((uint8_t)(s5 - n5f)) << 4);  // 801393AC subu ; 801393B0 sll 4
        c->mem_w8(0x800BF81Fu, v);                    // 801393B4 sb v0, -0x7e1(0x800c0000)
        // GOTCHA: NO branch between 0x801393B4 and 0x801393B8 — the node[5]==1 block
        // UNCONDITIONALLY falls through into the node[5]==2 block at 0x801393B8.
        goto jt1_0_eq2;
      }
      if (s5 >= 2) {                                  // ---- 0x8013934c ----
        // 8013934C beq a0,2 -> 0x801393b8 ; 80139354 beq a0,3 -> 0x801393dc ; else j tail
        if (s5 == 2) {
        jt1_0_eq2:
          // ---- 0x801393b8: node[5] == 2 ----
          c->r[2] = (uint32_t)eng(c).sceneTransition.stepSwapWaiter(obj);   // was rec_dispatch 0x80073328
          if (c->r[2] == 0) break;                    // 801393C0 beqz v0 -> 0x80139580 (tail)
          c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));  // 801393C8/D0/D8 lbu ; +1 ; sb 5(s0)
          break;                                      // 801393D4 j 0x80139580
        }
        if (s5 == 3) {
          // ---- 0x801393dc: node[5] == 3 ----
          if (c->mem_r8(S1 + 0x2a) == c->mem_r8(obj + 0x2a)) break;  // 801393DC lbu v1,0x2a(a1) ; 801393E0 lbu v0,0x2a(s0) ; 801393E8 beq -> tail
          c->mem_w8(obj + 5, 0);                      // 801393F4 sb zero, 5(s0)
          break;                                      // 801393F0 j 0x80139580
        }
        break;                                        // 8013935C j 0x80139580 (tail)
      }
      // ---- 0x80139364: node[5] == 0 ----
      if (c->mem_r8(S1 + 0x2a) != c->mem_r8(obj + 0x2a)) break;  // 80139364 lbu v1,0x2a(a1) ; 80139368 lbu v0,0x2a(s0) ; 80139370 bne -> tail
      if (c->mem_r8(S1 + 5) != 0) break;             // 80139378 lbu v0,5(a1) ; 80139380 bnez -> tail
      c->mem_w8(obj + 5, 1);                          // 8013938C sb a2(=1), 5(s0)
      c->mem_w8(0x1F800137u, 2);                      // 80139394 sb v0(=2), 0x137(0x1f800000)
      break;                                          // 80139390 j 0x80139580
    }

    case 1: {                                         // jt1[1] = 0x801393f8
      (void)eng(c).sceneTransition.stepSwapWaiter(obj);   // was rec_dispatch 0x80073328 (v0 discarded)
      eng(c).spawn.tickLinkedOverlay(obj, 0x45);   // 80139400/04/08 was rec_dispatch(0x800735F4u, a1=0x45)
      break;                                          // 8013940C j 0x80139580
    }

    case 2: {                                         // jt1[2] = 0x80139414
      (void)eng(c).sceneTransition.stepSwapWaiter(obj);   // was rec_dispatch 0x80073328 (v0 discarded)
      eng(c).spawn.tickLinkedOverlay(obj, 0x46);   // 8013941C/20/24 was rec_dispatch(0x800735F4u, a1=0x46)
      // 0x800c0000 - 0x7ec = 0x800BF814 ; v0 = lw & 0xffff0000 ; compare to 0x02010000
      uint32_t g = c->mem_r32(0x800BF814u) & 0xFFFF0000u;  // 8013942C lw v0 ; 80139434 and v0,v1(0xffff0000)
      if (g != 0x02010000u) {                         // 80139438 lui v1,0x201 ; 8013943C bne -> 0x80139470
        c->mem_w8(obj + 0x2a, 0x1f);                  // 80139470 sb v0(=0x1f), 0x2a(s0)  (v0=0x1f @80139440)
        c->mem_w8(obj + 0x5f, 0);                     // 80139474/78 sb zero, 0x5f(s0)
        break;                                        // 80139474 j 0x80139580
      }
      // g == 0x02010000:
      c->mem_w8(obj + 0x2a, 0x26);                    // 80139444/48 addiu v0,zero,0x26 ; sb v0, 0x2a(s0)
      // 0x800c0000 - 0x744 = 0x800BF8BC
      if (c->mem_r8(0x800BF8BCu) != 0xff) {           // 80139450 lbu v1,0x744(base) ; 80139458 bne v1,0xff -> 0x80139468
        c->mem_w8(obj + 0x5f, 1);                     // 80139468/6C sb v0(=1), 0x5f(s0)  (v0=1 @8013945C)
      } else {
        c->mem_w8(obj + 0x5f, 0);                     // 80139460/64 sb zero, 0x5f(s0)
      }
      break;                                          // 80139460 / 80139468 j 0x80139580
    }

    case 3: {                                         // jt1[3] = 0x8013947c : node[5] sub-machine
      uint8_t s5 = c->mem_r8(obj + 5);               // 8013947C lbu v1, 5(s0)
      // 80139484 beq v1,1 -> 0x801394d8 ; 8013948C slti v0,v1,2 ; 8013948C beqz -> v1>=2 (0x801394a4)
      if (s5 == 1) {
        // ---- 0x801394d8: node[5] == 1 ----
        uint8_t n6 = c->mem_r8(obj + 6);             // 801394D8 lbu v0, 6(s0)
        if (n6 == 0) {                               // 801394E0 beqz v0 -> 0x801394f8
          // ---- 0x801394f8 ----
          if (c->mem_r8(obj + 0x29) == 0) break;     // 801394F8 lbu v0,0x29(s0) ; 80139500 beqz -> tail
          c->mem_w8(obj + 6, (uint8_t)s5);           // 80139508 sb v1(=node[5]==1), 6(s0)
          // FUN_80054198(0x800E7E80) — SceneTransition::clearSwapBlock (native)
          eng(c).sceneTransition.clearSwapBlock(0x800E7E80u);
          c->r[4] = 0x6d; c->r[5] = 0x41;            // 80139518 a0=0x6d ; 80139520 a1=0x41
          rec_dispatch(c, 0x8004ED94u);              // 8013951C jal 0x8004ed94
          eng(c).sfx.trigger(0x19, 0, 0xF);       // 8013952C jal 0x80074590 (native)
          break;                                     // 80139534 j 0x80139580
        }
        if (n6 == s5) {                              // 801394E8 beq v0,v1 (v1==node[5]==1) -> 0x8013953c
          // ---- 0x8013953c ----
          if (c->mem_r8(obj + 0x29) != 0) break;     // 8013953C lbu v0,0x29(s0) ; 80139544 bnez -> tail
          // 0x800e0000 + 0x7fc7 = 0x800E7FC7
          uint8_t gbit = c->mem_r8(0x800E7FC7u) & 1; // 8013954C lbu v0,0x7fc7(0x800e) ; 80139554 andi 1
          if (gbit != c->mem_r8(obj + 0x5f)) break;  // 80139550 lbu v1,0x5f(s0) ; 80139558 bne v0,v1 -> tail
          c->mem_w8(obj + 6, 0);                      // 80139564 sb zero, 6(s0)
          break;                                      // 80139560 j 0x80139580
        }
        break;                                        // 801394F0 j 0x80139580 (n6 != 0 && n6 != 1)
      }
      if (s5 >= 2) {                                  // ---- 0x801394a4 ----
        if (s5 == 2) {                                // 801394A4 beq v1,2 -> 0x80139568 ; else tail
          // ---- 0x80139568 ----
          c->r[4] = obj; rec_dispatch(c, 0x80138B04u);// 80139568 jal 0x80138b04 (a0=s0)
          break;                                      // 80139570 j 0x80139580
        }
        break;                                        // 801394AC j 0x80139580
      }
      // ---- 0x801394b4: node[5] == 0 ----
      // 0x800c0000 - 0x625 = 0x800BF9DB
      if ((c->mem_r8(0x800BF9DBu) & 1) != 0) {       // 801394B4 lbu v0,0x9db(base) ; 801394BC andi 1 ; 801394C0 beqz -> 0x801394d0
        c->mem_w8(obj + 5, 2);                        // 801394C8/CC sb v0(=2), 5(s0)
      } else {
        c->mem_w8(obj + 5, 1);                        // 801394D0/D4 sb a0(=1), 5(s0)
      }
      break;                                          // 801394C8 / 801394D0 j 0x80139580
    }

    case 4:                                           // jt1[4] = 0x80139578
      c->r[4] = obj; rec_dispatch(c, 0x80138C70u);    // 80139578 jal 0x80138c70 (a0=s0)
      break;                                          // (no j; falls into tail @0x80139580)

    case 5:                                           // jt1[5] = 0x80139580 (== tail; no-op)
      break;
    }
  }

  // ---- common tail (0x80139580) ----
  if (c->mem_r8(obj + 1) != 0) {                      // 80139580 lbu v0,1(s0) ; 80139588 beqz -> 0x80139598
    c->r[4] = obj; eng(c).graphicsBind.renderUpdate();      // 80139590 jal 0x800517f8 (a0=s0)
  }
  c->mem_w8(obj + 0x29, 0);                           // 80139598 sb zero, 0x29(s0)
  c->mem_w8(obj + 0x2b, 0);                           // 801395A0 sb zero, 0x2b(s0)  (delay slot of j epilogue)
}
