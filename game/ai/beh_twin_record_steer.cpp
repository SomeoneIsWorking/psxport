// game/ai/beh_twin_record_steer.cpp — PC-native per-object BEHAVIOR handler FUN_80133D6C.
//
// Overlay handler (~x778/field-frame on seaside; ~180 instr), prologue 0x80133D6C; `jr ra` at
// 0x8013405C. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT. a0 was clobbered to 2 by the `beq a0,zero` delay slot, so node[8]=node[9]=2.
//             If mem[0x800ED098] (lh) < 2 -> node[4]=3 return. Else seed node[0x80]=node[0x82]=140,
//             node[0x84]=10, node[0x86]=70, node[4]=node[0]=1, node[13]=node[11]=0; allocate 2 child
//             records (FUN_8007AAE8) into node[0xC0]/node[0xC4]: rec0{[6]=-1,[0]=0}, rec1{[6]=0,
//             [0]=-140}, each rec[2]=rec[4]=rec[8]=rec[12]=0, then FUN_80051B04(rec,12,iter).
//   STATE 1 : steer the two child records toward targets that depend on node[0x29] (and, when ==1, on
//             FUN_800781E0 distance(scratch[0x160/0x164]-rec0[0x2C/0x34])): rec0[0x0C] steps ±5 toward
//             a1, rec1[0x0C] steps ±10 toward a3 (snap on overshoot). Then if mem[0x800E7EAA]<22 and
//             FUN_8007778C(node)!=0 -> FUN_800517F8(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). a0 fidelity in the
// record loop: guest a0 at the first FUN_8007AAE8 is 2 (the delay-slot clobber); FUN_80051B04 leaves
// a0=rec, so the 2nd FUN_8007AAE8 sees a0=rec0 — don't touch c->r[4] across the loop. Transcribed 1:1
// as a register machine; signed (lh/sra) vs unsigned (lhu/srl) preserved exactly; the step-toward-target
// snap stores the FULL 32-bit target truncated to 16 bits (sh), not its sign-extension. The byte-exact
// A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE.

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

constexpr uint32_t BEH_FN = 0x80133D6Cu;

// Step `cur` (sext16 current value) toward target `snap` by `step`, snapping on overshoot. Returns
// whether to store (false = cur already at target16, guest skips the sh) + the value to store.
static inline bool clamp_step(int32_t cur, uint32_t snap, int step, uint16_t* out) {
  int32_t tgt = (int16_t)(uint16_t)snap;            // a0 = sext16(snap)
  if (cur == tgt) return false;                     // beq -> no store
  int32_t v1;
  if (cur < tgt) {                                  // increase
    int32_t nv = cur + step;
    int32_t s = (int16_t)(uint16_t)nv;
    v1 = (tgt < s) ? (int32_t)snap : nv;            // a0 < sext16(nv) -> snap
  } else {                                          // decrease
    int32_t nv = cur - step;
    int32_t s = (int16_t)(uint16_t)nv;
    v1 = (s < tgt) ? (int32_t)snap : nv;            // sext16(nv) < a0 -> snap
  }
  *out = (uint16_t)v1;
  return true;
}

}  // namespace

void beh_twin_record_steer(Core* c) {
  uint32_t nd = c->r[4];                          // s2 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                // a0 = node[4] = outer state

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) { eng(c).spawn.despawn(nd); goto Lret; }
  goto Lret;

 // ================= STATE 0 (INIT) =================
 S0: {
   c->mem_w8(nd + 8, 2);                            // node[8] = a0 (clobbered to 2)
   if (c->mem_r16s(0x800ed098u) < 2) { c->mem_w8(nd + 4, 3); goto Lret; }
   c->mem_w16(nd + 0x82, 140);
   c->mem_w16(nd + 0x80, 140);
   c->mem_w16(nd + 0x84, 10);
   c->mem_w8(nd + 9, 2);                            // node[9] = a0 = 2
   c->mem_w8(nd + 4, 1);
   c->mem_w8(nd + 0, 1);
   c->mem_w8(nd + 13, 0);
   c->mem_w8(nd + 11, 0);
   c->mem_w16(nd + 0x86, 70);
   c->r[4] = 2;                                     // mirror guest a0 for the first FUN_8007AAE8
   for (int iter = 0; iter < 2; iter++) {
     eng(c).graphicsBind.recordAlloc();                  // FUN_8007AAE8() -> v0 (alloc); a0 carried from prior
     uint32_t rec = c->r[2];
     c->mem_w32(nd + 0xc0 + 4 * iter, rec);
     c->mem_w16(rec + 6, (uint16_t)(int16_t)(iter - 1));   // rec[6] = s1-1 (-1, then 0)
     c->mem_w16(rec + 0, (uint16_t)(int16_t)(iter == 0 ? 0 : -140));
     c->mem_w16(rec + 2, 0);
     c->mem_w16(rec + 4, 0);
     c->mem_w32(rec + 8, 0);
     c->mem_w32(rec + 12, 0);
     // FUN_80051B04(rec, 12, iter); leaves a0=rec for the next FUN_8007AAE8
     eng(c).graphicsBind.installSceneRecord(rec, 12, (uint32_t)iter);                // FUN_80051B04 (native)
   }
   goto Lret;
 }

 // ================= STATE 1 =================
 S1: {
   uint32_t n29 = c->mem_r8(nd + 0x29);
   uint32_t a1v, a3v;
   if (n29 == 1) {
     uint32_t rec0 = c->mem_r32(nd + 0xc0);
     int32_t aa = c->mem_r16s(0x1f800160u) - (int32_t)c->mem_r32(rec0 + 0x2c);
     int32_t bb = c->mem_r16s(0x1f800164u) - (int32_t)c->mem_r32(rec0 + 0x34);
     int32_t v1 = (int32_t)guest_leaf(c, 0x800781e0u, (uint32_t)aa, (uint32_t)bb);   // FUN_800781E0
     if (v1 < 60)        { a1v = 0; a3v = 0; }
     else if (v1 < 140)  { a1v = ((uint32_t)(-v1)) >> 1; a3v = 0; }
     else                { a1v = (uint32_t)(int32_t)-70; a3v = ((uint32_t)(140 - v1)) >> 1; }
   } else if (n29 == 0)  { a1v = 0; a3v = 0; }
   else if (n29 == 2)    { a1v = (uint32_t)(int32_t)-70; a3v = (uint32_t)(int32_t)-70; }
   else                  { a1v = 0; a3v = 0; }      // n29 >= 3

   uint32_t rec0 = c->mem_r32(nd + 0xc0);
   uint16_t nv;
   if (clamp_step(c->mem_r16s(rec0 + 0x0c), a1v, 5, &nv))
     c->mem_w16(c->mem_r32(nd + 0xc0) + 0x0c, nv);
   uint32_t rec1 = c->mem_r32(nd + 0xc4);
   if (clamp_step(c->mem_r16s(rec1 + 0x0c), a3v, 10, &nv))
     c->mem_w16(c->mem_r32(nd + 0xc4) + 0x0c, nv);

   if (c->mem_r8(0x800e7eaau) < 22) {
     if (Actor(c, nd).boundsCull() != 0) {             // FUN_8007778C — Actor::boundsCull
       c->r[4] = nd; eng(c).graphicsBind.renderUpdate();                   // FUN_800517F8(node)
     }
   }
   goto Lret;
 }

 Lret:
  return;
}
