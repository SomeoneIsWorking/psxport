// game/ai/beh_quad_record_table_seed.cpp — PC-native per-object BEHAVIOR handler FUN_80135D64.
//
// Overlay handler (~x1552/field-frame on seaside; ~230 instr), prologue 0x80135D64; `jr ra` at
// 0x801360EC. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT. If node[3]<2 and global 0x800ED098 >= 4, allocate node[8]=4 child records via
//             FUN_8007AAE8, each filled from the per-node[3] source table @0x8014A780 (8 bytes/rec)
//             + FUN_80051B04. (If 0x800ED098 < 4 the loop is skipped; node[3]>=2 skips it too.) Then
//             seed the common block: node[4]=node[0]=1, node[0x80..0x86]=30/60/50/100, node[0x2E/0x32/
//             0x36/0x56] from tbl2 @0x8014A7A0[node[3]] (8 bytes/rec), node[5/6/13/11/0x29/0x54/0x58]=0,
//             node[0x48]=512, node[0x4A]=50, node[0x60]=node[0x32]; if node[3]==0 and node[0x2E] <
//             scratch[0x160], node[0x2E]=mem[0x8014A7BA]. Then FUN_80135414(node)+FUN_800517F8(node).
//   STATE 1 : compute a gate `a0` (node[3]==0 && mem[0x800E7E84]==770 -> scratch[0x160]<mem[0x8014A7BA];
//             OR mem[0x800E7EAA] in {28,38}); if set, reload node[0x2E/0x32/0x36] from tbl2. Then gate
//             on scratchpad[0x207] (>=22, the ==23 sub-case needs scratch[0xDA]>=11000) and
//             mem[0x800E7EAA]<32; if FUN_8007778C(node)!=0 -> FUN_80135414(node)+FUN_800517F8(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). a0 fidelity in the
// record loop kept by NOT clobbering c->r[4] between FUN_80051B04 (leaves the guest a0=rec) and the next
// FUN_8007AAE8 (reads it). The transient node[4]=3 store on the 0x800ED098<4 path is immediately
// overwritten by node[4]=1 at the common tail — so the END RAM has node[4]=1 either way (mirrored).
// Transcribed 1:1 as a register machine; signed (lh/slt) vs unsigned (lhu/lbu) preserved. The byte-exact
// A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

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

constexpr uint32_t BEH_FN = 0x80135D64u;

}  // namespace

void beh_quad_record_table_seed(Core* c) {
  uint32_t nd = c->r[4];                          // s2 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                // node[4] = outer state

  if (st == 1) goto L5fa4;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) { eng(c).spawn.despawn(nd); goto Lret; }
  goto Lret;

 // ================= STATE 0 (INIT) =================
 S0: {
   uint8_t n3 = c->mem_r8(nd + 3);
   if ((int32_t)n3 >= 2) goto L5eac;               // node[3]>=2 -> skip alloc loop (node[4]=1 at tail)
   if ((int32_t)n3 < 0) goto L5eac;                // bltz (dead: lbu is 0..255)
   if (c->mem_r16s(0x800ed098u) < 4) { c->mem_w8(nd + 4, 3); goto L5eac; }  // transient =3
   // alloc loop: node[8]=node[9]=4 records
   c->mem_w8(nd + 8, 4);
   c->mem_w8(nd + 9, 4);
   if (c->mem_r8(nd + 8) != 0) {                   // (always; andi 4 != 0)
     uint32_t s4 = 0x8014a780u;                     // src table base, 8 bytes/record
     c->r[4] = nd;                                  // mirror guest a0 for first FUN_8007AAE8
     int s3 = 0;
     uint32_t s0 = nd;                              // node + i*4
     do {
       eng(c).graphicsBind.recordAlloc();                // FUN_8007AAE8() -> v0 (alloc); a0 = guest a0
       uint32_t rec = c->r[2];
       s3 += 1;
       c->mem_w32(s0 + 0xc0, rec);
       c->mem_w16(rec + 6, (uint16_t)(int16_t)-1);
       c->mem_w16(rec + 0, c->mem_r16(s4 + 0));
       c->mem_w16(rec + 2, c->mem_r16(s4 + 2));
       c->mem_w16(rec + 4, c->mem_r16(s4 + 4));
       c->mem_w32(rec + 8, 0);
       c->mem_w32(rec + 12, 0);
       uint32_t a2 = (uint32_t)c->mem_r16s(s4 + 6);
       guest_leaf(c, 0x80051b04u, rec, 12, a2);      // FUN_80051B04(rec, 12, (int16)src[6])
       s4 += 8;
       s0 += 4;
     } while ((int32_t)s3 < (int32_t)c->mem_r8(nd + 8));
   }
   goto L5eac;
 }

 // ================= STATE 0 common tail =================
 L5eac: {
   c->mem_w8(nd + 4, 1);
   c->mem_w8(nd + 0, 1);
   c->mem_w16(nd + 0x80, 30);
   c->mem_w16(nd + 0x82, 60);
   c->mem_w16(nd + 0x84, 50);
   c->mem_w16(nd + 0x86, 100);
   c->mem_w8(nd + 5, 0);
   c->mem_w8(nd + 6, 0);
   c->mem_w8(nd + 13, 0);
   c->mem_w8(nd + 11, 0);
   c->mem_w8(nd + 0x29, 0);
   uint8_t n3 = c->mem_r8(nd + 3);
   uint32_t e = 0x8014a7a0u + (uint32_t)n3 * 8u;    // tbl2[node[3]]
   c->mem_w16(nd + 0x2e, c->mem_r16(e + 0));
   c->mem_w16(nd + 0x32, c->mem_r16(e + 2));
   c->mem_w16(nd + 0x54, 0);
   c->mem_w16(nd + 0x36, c->mem_r16(e + 4));
   c->mem_w16(nd + 0x58, 0);
   c->mem_w16(nd + 0x56, c->mem_r16(e + 6));
   if (n3 == 0) {
     if (c->mem_r16s(nd + 0x2e) < c->mem_r16s(0x1f800160u))
       c->mem_w16(nd + 0x2e, c->mem_r16(0x8014a7bau));
   }
   // L5f80:
   uint16_t v32 = c->mem_r16(nd + 0x32);
   c->mem_w16(nd + 0x48, 512);
   c->mem_w16(nd + 0x4a, 0);
   c->mem_w16(nd + 0x4a, 50);
   c->mem_w16(nd + 0x60, v32);                      // node[0x60] = node[0x32] (delay slot)
   goto L60b4;
 }

 // ================= STATE 1 =================
 L5fa4: {
   uint32_t a0 = 0;
   uint8_t n3 = c->mem_r8(nd + 3);
   if (n3 == 0) {
     if (c->mem_r16(0x800e7e84u) == 770) {
       int16_t x = c->mem_r16s(0x1f800160u);
       int16_t y = c->mem_r16s(0x8014a7bau);
       a0 = (x < y) ? 1u : 0u;
     }
   }
   uint8_t ev = c->mem_r8(0x800e7eaau);
   if (ev == 28 || ev == 38) a0 = 1;
   if (a0 != 0) {                                   // reload tbl2 fields
     uint8_t nn = c->mem_r8(nd + 3);
     uint32_t e = 0x8014a7a0u + (uint32_t)nn * 8u;
     c->mem_w16(nd + 0x2e, c->mem_r16(e + 0));
     c->mem_w16(nd + 0x32, c->mem_r16(e + 2));
     c->mem_w16(nd + 0x36, c->mem_r16(e + 4));
   }
   // L6054: scratchpad gate
   uint8_t sp = c->mem_r8(0x1f800207u);
   if (sp < 22) goto Lret;
   if (sp == 23) { if (c->mem_r16s(0x1f8000dau) < 11000) goto Lret; }
   // L608c:
   if (!(c->mem_r8(0x800e7eaau) < 32)) goto Lret;
   if (guest_leaf(c, 0x8007778cu, nd) == 0) goto Lret;
   goto L60b4;
 }

 // ================= shared tail (state 0 & 1) =================
 L60b4:
  guest_leaf(c, 0x80135414u, nd);                    // FUN_80135414(node)
  c->r[4] = nd; eng(c).graphicsBind.renderUpdate();                          // FUN_800517F8(node)
 Lret:
  return;
}
