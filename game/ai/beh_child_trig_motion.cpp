// game/ai/beh_child_trig_motion.cpp — PC-native per-object BEHAVIOR handler FUN_8013A900.
//
// Overlay handler (~x1554/field-frame on seaside; ~205 instr), prologue 0x8013A900; `jr ra` at
// 0x8013AC2C. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT — gate on global 0x800ED098 (<2 -> node[4]=3); else seed node[8]=node[9]=2,
//             node[0x80..0x86]=30/60/50/100, node[4]=node[0]=1, node[0x29]=node[13]=node[11]=0; then
//             allocate node[8] child records via FUN_8007AAE8, fill each from the per-node[3] source
//             table @0x8014AAB0[node[3]] (8 bytes/record) + FUN_80051B04; then per node[3]: 0 -> gate
//             0x800BF8F7 then FUN_801252C0(node,5,2)+(node,5,3); 2 -> seed node[0x2E..0x60] motion fields.
//   STATE 1 : gate scratchpad[0x207] in [23,31] then FUN_8007778C; per node[3]: 0 -> a trig block
//             (FUN_80083E80/FUN_80083F50 of the 0x800E7ED8 angle, FUN_80085690 x2, write node[0xC4]
//             record fields 0/2/8/10); 1/2 -> FUN_80135414; then FUN_800517F8(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record WRITES owned
// native; every sub-behavior CALL (incl. the trig/math helpers, which are ordinary rec_dispatch leaves
// returning via v0 — NOT gte_op) stays PSX. a0 fidelity in the record loop is kept by NOT clobbering
// c->r[4] between FUN_80051B04 (which leaves the guest a0) and the next FUN_8007AAE8 (which reads it).
// Source/data tables read live from resident overlay RAM. No leaf takes a stack arg. Transcribed 1:1;
// the byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "trig.h"    // class Trig — libgte rsin/rcos
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013A900u;

static inline uint32_t leafr(Core* c, uint32_t a0, uint32_t fn) {  // returns v0
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}
static inline uint32_t leafr2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {  // returns v0
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_child_trig_motion(Core* c) {
  uint32_t nd = c->r[4];                             // s3 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                   // node[4] = outer state

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;                            // STATE 2 nothing
  if (st == 3) { eng(c).spawn.despawn(nd); goto Lret; }   // STATE 3
  goto Lret;                                         // st >= 4

 // ================= STATE 0 (INIT) =================
 S0:
  if (c->mem_r16s(0x800ED098u) < 2) { c->mem_w8(nd + 4, 3); goto Lret; }  // 0x800ED098 gate
  c->mem_w8 (nd + 8, 2);
  c->mem_w8 (nd + 9, 2);
  c->mem_w16(nd + 0x80, 30);
  c->mem_w16(nd + 0x82, 60);
  c->mem_w16(nd + 0x84, 50);
  c->mem_w16(nd + 0x86, 100);
  c->mem_w8 (nd + 4, 1);                             // node[4] = 1 (a0)
  c->mem_w8 (nd + 0, 1);                             // node[0] = 1 (a0)
  c->mem_w8 (nd + 0x29, 0);
  c->mem_w8 (nd + 13, 0);
  c->mem_w8 (nd + 11, 0);
  {
    uint8_t n3 = c->mem_r8(nd + 3);
    uint32_t src = c->mem_r32(0x8014AAB0u + (uint32_t)n3 * 4u);  // s0 = table[node[3]]
    if (c->mem_r8(nd + 8) != 0) {
      c->r[4] = 1;                                   // mirror guest a0 = 1 for the first FUN_8007AAE8
      int s2 = 0;                                    // record index
      uint32_t s1 = nd;                              // node + i*4
      do {
        eng(c).graphicsBind.recordAlloc();                // FUN_8007AAE8() -> v0 (alloc); a0 = guest a0
        uint32_t rec = c->r[2];
        c->mem_w32(s1 + 0xC0, rec);
        c->mem_w16(rec + 6, (uint16_t)(s2 - 1));
        c->mem_w16(rec + 0, c->mem_r16(src)); src += 2;
        c->mem_w16(rec + 2, c->mem_r16(src)); src += 2;
        c->mem_w16(rec + 4, c->mem_r16(src));
        c->mem_w32(rec + 8, 0);
        src += 2;
        c->mem_w32(rec + 12, 0);
        uint32_t a2 = (uint32_t)c->mem_r16s(src);
        guest_leaf(c, 0x80051B04u, rec, 12, a2);          // FUN_80051B04(rec, 12, (int16)*src)
        src += 2;
        s2 += 1;
        s1 += 4;
      } while ((int32_t)s2 < (int32_t)c->mem_r8(nd + 8));
    }
  }
  {
    uint8_t n3 = c->mem_r8(nd + 3);
    if (n3 == 0) {                                   // 0x8013aa84
      if (c->mem_r8(0x800BF8F7u) == 0) goto Lret;
      guest_leaf(c, 0x801252C0u, nd, 5, 2);              // FUN_801252C0(node, 5, 2)
      guest_leaf(c, 0x801252C0u, nd, 5, 3);              // FUN_801252C0(node, 5, 3)
      goto Lret;
    }
    if (n3 == 2) {                                   // 0x8013aabc
      c->mem_w16(nd + 0x2E, 20084);
      c->mem_w16(nd + 0x32, (uint16_t)(int16_t)-3728);
      uint16_t v60 = c->mem_r16(nd + 0x32);          // lhu node[0x32]
      c->mem_w16(nd + 0x36, 8836);
      c->mem_w16(nd + 0x54, 0);
      c->mem_w16(nd + 0x56, 3072);
      c->mem_w16(nd + 0x58, 0);
      c->mem_w16(nd + 0x4A, 0);
      c->mem_w16(nd + 0x60, v60);                    // node[0x60] = node[0x32] (delay slot)
      goto Lret;
    }
    goto Lret;                                       // node[3] == 1 or >= 3
  }

 // ================= STATE 1 =================
 S1: {
    uint8_t sp = c->mem_r8(0x1F800207u);             // scratchpad byte
    if ((uint32_t)(sp - 23) >= 9) goto Lret;         // sp not in [23,31]
    if (leafr(c, nd, 0x8007778Cu) == 0) goto Lret;   // FUN_8007778C(node)
    uint8_t n3 = c->mem_r8(nd + 3);
    if (n3 != 0) {
      if ((int32_t)(int8_t)n3 < 0) goto L8c00;       // bltz (never true for lbu)
      if (n3 >= 3) goto L8c00;
      // n3 in {1,2}: FUN_80135414
      guest_leaf(c, 0x80135414u, nd);
      goto L8c00;
    }
    // n3 == 0: trig block @0x8013ab44
    if (c->mem_r8(nd + 0x29) == 0) goto L8c00;       // node[0x29]
    {
      int32_t ang;
      if (c->mem_r8(0x800E7FC7u) & 1) ang = -c->mem_r16s(0x800E7ED8u);
      else                            ang =  c->mem_r16s(0x800E7ED8u);
      uint32_t v0e = (uint32_t)trigOf(c).rsin(ang);   // FUN_80083E80(ang) -> native Trig::rsin
      uint32_t v0f = (uint32_t)trigOf(c).rcos(ang);   // FUN_80083F50(ang) -> native Trig::rcos
      int32_t s0 = (-(int32_t)v0e) >> 9;             // sra 9 (arithmetic)
      int32_t s1 =  ((int32_t)v0f) >> 9;
      uint32_t vA = (uint32_t)trigOf(c).ratan2(s1, 110);   // FUN_80085690 -> native Trig::ratan2
      uint32_t vB = (uint32_t)trigOf(c).ratan2(s0, 110);
      uint32_t rec = c->mem_r32(nd + 0xC4);
      c->mem_w16(rec + 0, (uint16_t)(c->mem_r16(0x8014AA98u) + (uint32_t)s0));
      c->mem_w16(rec + 2, (uint16_t)(c->mem_r16(0x8014AA9Au) + (uint32_t)s1));
      c->mem_w16(rec + 8, (uint16_t)vA);
      c->mem_w16(rec + 10, (uint16_t)(0u - vB));
      goto L8c00;
    }
  }

 L8c00:
  c->r[4] = nd; eng(c).graphicsBind.renderUpdate();                          // FUN_800517F8(node)
 Lret:
  return;
}
