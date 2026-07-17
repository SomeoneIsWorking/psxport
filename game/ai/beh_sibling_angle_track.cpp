// game/ai/beh_sibling_angle_track.cpp — PC-native per-object BEHAVIOR handler FUN_801395C0 (OVERLAY).
//
// An OVERLAY-resident per-object behavior routine (lives in the field overlay, immediately AFTER its
// sibling FUN_80138FC8 — its epilogue jr ra is at 0x80139830 and the next function has its own prologue
// at 0x80139838). Installed at node+0x1c and called every frame by the entity walk with the object node
// in a0. Same SHAPE as the resident/overlay siblings (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler /
// the FUN_80124e74 handler / the FUN_80138fc8 handler): a state machine on the node's state byte node[4] (0 init / 1 active /
// 2 idle / 3 despawn). It loads s1 = node[0x10] (a sibling/parent object-node pointer) up front and uses
// it heavily in both states.
//
//   state 0 -> FUN_80051b70 cull-record init (a1=0xc, a2=0x16). If it returns !=0 the init is busy and we
//              bail. On success: state++, then seed node[0x2e]/0x32/0x36 from the shared per-type struct
//              table TD (8-byte stride) indexed by (s1[3]*2 + node[3]), clear node[0x54]/node[0x58], and
//              set node[0x56] = (s1[3] != 0) ? 0x400 : 0.
//   state 1 -> a global gate on scratchpad[0x207]: only proceed when scratchpad[0x207] >= 0x17 AND != 0x22.
//              Then a 4-way branch keyed on (s1[3]!=0, node[3]!=0) computes a delta pair (a1, v0) from
//              s1's box fields (0x4e/0x52/0x80/0x82/0x84/0x86) and the node's 0x2e/0x36, divides, calls the
//              angle helper FUN_80083e80, derives node[0x32] = s1[0x32] + ((s1[0x50]-s1[0x32]) * trig >> 12)
//              - 0x60, then the cull FUN_8007778c; if its result != 0, FUN_800517f8 (render).
//   state 2 -> return (idle)
//   state 3 -> FUN_8007a624 (despawn)
//
// Per-type data table it READS from guest RAM (NOT hardcoded — read live with mem_r*, exactly as the recomp
// body does):
//   TD @0x8014AA38  struct[(s1[3]*2 + node[3])*8]: lhu@0 -> node[0x2e], lhu@2 -> node[0x32], lhu@4 ->
//                   node[0x36]  (the SAME table the sibling 0x80138FC8 reads in its JT1 case 2)
//
// Ownership model (identical to the siblings): CONTROL FLOW + node/global memory writes owned native; every
// sub-behavior CALL stays a reachable PSX leaf via rec_dispatch (NO recursion). NO GTE, NO render packets
// here. RE'd 1:1 from disas 0x801395C0..0x80139834 (epilogue jr ra @0x80139830). It WRITES guest node state
// the still-recomp content reads -> content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B vs
// rec_super_call). The idle/active field path is exercised by the gate; the scene-driven sub-states are
// faithfully transcribed and verify when a scene drives them (same caveat as the sibling orchestrators).

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_record_init
#include "trig.h"            // class Trig — libgte rsin/rcos
#include "object/actor.h"    // class Actor — Actor::boundsCull (FUN_8007778C native)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x801395C0u;

constexpr uint32_t TD = 0x8014AA38u;   // lui 0x8015, addiu -0x55c8  (8-byte stride)

}  // namespace

void beh_sibling_angle_track(Core* c) {
  const uint32_t obj = c->r[4];                         // 801395C8  move s2, a0
  uint8_t st = c->mem_r8(obj + 4);                      // 801395DC  lbu v1, 4(s2)
  const uint32_t s1 = c->mem_r32(obj + 0x10);           // 801395E0  lw  s1, 0x10(s2)

  // ---- state dispatch [0x801395E4..0x80139618] ----
  // 801395E4 beq v1,1 -> state1 (0x801396d8) ; 801395E8 slti v0,v1,2 ; 801395EC beqz -> v1>=2 (0x80139604)
  if (st != 1) {
    if (st >= 2) {                                      // ---- 0x80139604 ----
      // 80139608 beq v1,2 -> epilogue (0x80139820) ; 80139610 beq v1,3 -> despawn (0x80139818)
      if (st == 2) return;                              // idle
      if (st == 3) {                                    // 0x80139818: despawn
        eng(c).spawn.despawn(obj);    // 80139818 jal 0x8007a624 (a0=s2)
      }
      return;                                           // 80139618 j 0x80139820 (epilogue, no-op)
    }
    if (st != 0) return;                                // 801395F4 beqz v1 -> state0 ; else 801395FC j 0x80139820

    // ============================================================================================
    // STATE 0 (0x80139620): cull-record init + per-type field seed from TD
    // ============================================================================================
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = 0x16;       // 80139620 a1=0xc ; 80139628 a2=0x16 (delay) ; a0=s2
    eng(c).graphicsBind.recordInit();                       // 80139624 jal 0x80051b70
    if (c->r[2] != 0) return;                           // 8013962C bnez v0 -> 0x80139820 (init busy)

    c->mem_w8(obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));  // 80139638/40/44 lbu v0,4 ; +1 ; sb -> state 1

    // a0 = TD base 0x8014AA38 ; idx = (s1[3]*2 + node[3]) ; e = TD + idx*8
    uint8_t n3 = c->mem_r8(obj + 3);                    // 8013964C lbu v1, 3(s2)  (node3, reused below)
    auto td_e = [&](void) -> uint32_t {                 // recompute exactly as the body does each time
      uint32_t s1n3 = c->mem_r8(s1 + 3);                // lbu v0, 3(s1)
      uint32_t idx  = (s1n3 << 1) + n3;                 // sll v0,1 ; addu v0,v1
      return TD + (idx << 3);                           // sll v0,3 ; addu v0,a0
    };
    c->mem_w16(obj + 0x2e, c->mem_r16(td_e() + 0));     // 80139660/68 lhu v0,(e) ; sh v0, 0x2e(s2)
    c->mem_w16(obj + 0x32, c->mem_r16(td_e() + 2));     // 80139684/8C lhu v0,2(e) ; sh v0, 0x32(s2)
    uint16_t e4 = c->mem_r16(td_e() + 4);               // 801396A8 lhu v0, 4(e)
    c->mem_w16(obj + 0x54, 0);                          // 801396AC sh zero, 0x54(s2)
    c->mem_w16(obj + 0x36, e4);                         // 801396B0 sh v0, 0x36(s2)

    // node[0x56] = (s1[3] != 0) ? 0x400 : 0 ; node[0x58] = 0
    // GOTCHA: the 0x400 const at 0x801396C0 is the delay slot of bnez and is computed unconditionally,
    // but when s1[3]==0 the store at 0x801396C8 writes ZERO (sh zero), not 0x400.
    if (c->mem_r8(s1 + 3) != 0) {                       // 801396B4 lbu v0,3(s1) ; 801396BC bnez -> 0x801396cc
      c->mem_w16(obj + 0x56, 0x400);                    // 801396CC sh v0(=0x400), 0x56(s2)
    } else {
      c->mem_w16(obj + 0x56, 0);                        // 801396C8 sh zero, 0x56(s2)  (delay slot of j)
    }
    c->mem_w16(obj + 0x58, 0);                          // 801396D4 sh zero, 0x58(s2)  (delay slot of j epilogue)
    return;                                             // 801396D0 j 0x80139820
  }

  // ============================================================================================
  // STATE 1 (0x801396d8): scratchpad gate, box-delta -> angle, derive node[0x32], cull, render
  // ============================================================================================
  // 801396D8 lui v0,0x1f80 ; 801396DC lbu v1, 0x207(0x1f800000)
  uint8_t g = c->mem_r8(0x1F800207u);                   // 801396DC lbu v1, 0x207
  if (g < 0x17) return;                                 // 801396E4 sltiu v0,v1,0x17 ; 801396E8 bnez -> epilogue
  if (g == 0x22) return;                                // 801396EC v0=0x22 ; 801396F0 beq v1,0x22 -> epilogue

  // 4-way branch on (s1[3]!=0, node[3]!=0) -> compute (a1, v0) deltas. Two join points: a1 set per path,
  // then v0 computed at the join (some paths via the shared subu at 0x80139764).
  int16_t a1;                                           // a1 (after the path-specific subu)
  int16_t vv;                                           // v0 (the second delta) at the join
  bool s1n3 = (c->mem_r8(s1 + 3) != 0);                 // 801396F8 lbu v0,3(s1) ; 80139700 bnez -> 0x80139740
  bool n3nz = (c->mem_r8(obj + 3) != 0);                // (node3) 80139708/80139740 lbu v0,3(s2)
  if (!s1n3) {
    if (!n3nz) {                                        // ---- 0x80139718: s1[3]==0, node3==0 ----
      uint16_t v_4e = c->mem_r16(s1 + 0x4e);            // 80139718 lhu v0, 0x4e(s1)
      uint16_t v_80 = c->mem_r16(s1 + 0x80);            // 8013971C lhu v1, 0x80(s1)
      uint16_t v_2e = c->mem_r16(obj + 0x2e);           // 80139720 lhu a0, 0x2e(s2)
      a1 = (int16_t)(uint16_t)(v_4e - v_80);            // 80139728 a1 = s1[0x4e] - s1[0x80]  (delay slot)
      vv = (int16_t)(uint16_t)(v_2e - v_80);            // 80139764 v0 = a0 - v1 = node[0x2e] - s1[0x80]
    } else {                                            // ---- 0x8013972c: s1[3]==0, node3!=0 ----
      uint16_t v_82 = c->mem_r16(s1 + 0x82);            // 8013972C lhu v0, 0x82(s1)
      uint16_t v_4e = c->mem_r16(s1 + 0x4e);            // 80139730 lhu v1, 0x4e(s1)
      uint16_t v_2e = c->mem_r16(obj + 0x2e);           // 80139734 lhu a0, 0x2e(s2)
      a1 = (int16_t)(uint16_t)(v_82 - v_4e);            // 8013973C a1 = s1[0x82] - s1[0x4e]  (delay slot)
      vv = (int16_t)(uint16_t)(v_82 - v_2e);            // 80139778 v0 = v0 - a0 = s1[0x82] - node[0x2e]
    }
  } else {
    if (!n3nz) {                                        // ---- 0x80139750: s1[3]!=0, node3==0 ----
      uint16_t v_52 = c->mem_r16(s1 + 0x52);            // 80139750 lhu v0, 0x52(s1)
      uint16_t v_84 = c->mem_r16(s1 + 0x84);            // 80139754 lhu v1, 0x84(s1)
      uint16_t v_36 = c->mem_r16(obj + 0x36);           // 80139758 lhu a0, 0x36(s2)
      a1 = (int16_t)(uint16_t)(v_52 - v_84);            // 8013975C a1 = s1[0x52] - s1[0x84]
      vv = (int16_t)(uint16_t)(v_36 - v_84);            // 80139764 v0 = a0 - v1 = node[0x36] - s1[0x84]
    } else {                                            // ---- 0x80139768: s1[3]!=0, node3!=0 ----
      uint16_t v_86 = c->mem_r16(s1 + 0x86);            // 80139768 lhu v0, 0x86(s1)
      uint16_t v_52 = c->mem_r16(s1 + 0x52);            // 8013976C lhu v1, 0x52(s1)
      uint16_t v_36 = c->mem_r16(obj + 0x36);           // 80139770 lhu a0, 0x36(s2)
      a1 = (int16_t)(uint16_t)(v_86 - v_52);            // 80139774 a1 = s1[0x86] - s1[0x52]
      vv = (int16_t)(uint16_t)(v_86 - v_36);            // 80139778 v0 = v0 - a0 = s1[0x86] - node[0x36]
    }
  }

  // ---- join 0x8013977c: a0 = (int16)vv << 10 ; divisor = (int16)a1 ; quotient -> angle helper ----
  int32_t num = (int32_t)vv << 10;                      // 8013977C sll a0,16 ; 80139780 sra a0,6  (= vv<<10)
  int32_t den = (int32_t)a1;                            // 80139784 sll v0,16 ; 80139788 sra v0,16 (= (int16)a1)
  // 8013978C div zero,a0,v0 ; the break 7 / break 6 are the compiler's div-by-zero / overflow traps —
  // they do not fire in normal play. Replicate MIPS truncating division.
  int32_t quot = (den != 0) ? (num / den) : 0;          // 801397B4 mflo a0 (quotient)

  int16_t s0 = (int16_t)(uint16_t)(c->mem_r16(s1 + 0x50) - c->mem_r16(s1 + 0x32));  // 801397B8/BC/C4 s0 = s1[0x50]-s1[0x32]
  int32_t trig = trigOf(c).rsin((int32_t)(int16_t)quot);  // 801397CC jal 0x80083e80 -> native class Trig::rsin

  int64_t prod = (int64_t)(int32_t)s0 * (int32_t)trig; // 801397DC mult s0,v0 (s0 sign-extended @801397D4/D8)
  int32_t v1 = (int32_t)(prod >> 12);                   // 801397EC sra v1,0xc  (mflo >> 12)
  uint16_t n32 = c->mem_r16(s1 + 0x32);                 // 801397E4 lhu v0, 0x32(s1)
  int32_t newv = (int32_t)(int16_t)n32 + v1 - 0x60;     // 801397F0 addu ; 801397F4 -0x60
  // GOTCHA: node[0x32] store is the DELAY SLOT of jal 0x8007778c (0x801397F8); the recomp stores
  // newv (computed above) BEFORE the call runs. Keep that ordering here too — the cull body reads
  // posY (obj+0x32) so the write must land first.
  c->mem_w16(obj + 0x32, (uint16_t)newv);              // 801397FC sh v0, 0x32(s2)  (delay slot)
  if (Actor(c, obj).boundsCull() == 0) return;         // FUN_8007778C native (was rec_dispatch)
  c->r[4] = obj; eng(c).graphicsBind.renderUpdate();          // 80139808 jal 0x800517f8 (render; a0=s2 delay)
  // 80139810 j 0x80139820 (epilogue)
}
