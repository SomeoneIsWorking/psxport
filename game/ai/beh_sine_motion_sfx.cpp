// game/ai/beh_sine_motion_sfx.cpp — PC-native per-object BEHAVIOR handler FUN_80136158.
//
// Overlay handler (~x778/field-frame on seaside; ~290 instr), prologue 0x80136158; `jr ra` at
// 0x801365BC. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT. v0=FUN_80051B70(node,12,6); bail if !=0. Else node[0x80]=576, node[0x82]=1152,
//             node[0x29]=0, node[0]|=1, FUN_8004766C(node), FUN_80048750(node); seed node[0x54/0x58/0x62]=0,
//             node[4]=1, node[5]=0, node[0x68]=0, node[0x56]=scratch[0x1A0], node[0x90/0x92/0x94]=
//             node[0x2E/0x32/0x36]. Falls THROUGH into STATE 1.
//   STATE 1 : inner node[5] machine. N5∈{0,1} (inner A): a "should-run" gate on scratch[0x207]/mem[0x800BF816];
//             if it passes, init the record + node[5]=2, then fall into the MATH block. N5==2: the MATH block.
//             N5>=3: return. MATH block (only when mem[0x800BF809]==0): clamp node[0x44] toward a window,
//             derive an angular step s1/s2 (with rounding divides), sin(s2) via FUN_80083E80 scaled by
//             832>>12 added to node[0x94] -> node[0x36] (and mem[0x800E7EB6] accumulate when node[0x29]==1);
//             store rec[0x0C]=s2, node[0x44]=s1, node[0x29]=0. POST-MATH: same should-run gate -> node[5]=1
//             + return, else mem[0x800E7EAA]-keyed FUN_8007703C/FUN_8007778C; if node[1]!=0 FUN_800517F8 +
//             (when mem[0x800BF809]==0) velocity-magnitude SFX via FUN_80074AF0/FUN_80074590. Tail stores
//             node[0x64]=s1.
//   STATE 2/3 : FUN_8007A624(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). Transcribed 1:1 as a
// register machine with `goto L<hex>` = guest addresses — the ONLY reliable way through the delay-slot
// subtleties (branch conditions read the PRE-delay reg; delay-slot writes/loads still execute). Signed
// (lh/sra) vs unsigned (lhu/srl/sltiu) preserved exactly; shifts masked to 32-bit. FUN_80083E80 is the
// owned sin leaf (returns v0). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the
// safety net. NO GTE (the sin is a table lookup, not gte_op).

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::enqueueByClass (FUN_8007703C)
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
#include "math/trig.h"   // class Trig — rsin (FUN_80083E80)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80136158u;

}  // namespace

void beh_sine_motion_sfx(Core* c) {
  uint32_t nd = c->r[4];                 // s0 (node) — constant
  int32_t s1 = 0, s2 = 0, s3 = 0;
  int32_t v0, v1, a0;
  bool d1, d2, d3;

  v1 = (int32_t)c->mem_r8(nd + 4);       // lbu node[4]
  s2 = 1;
  if (v1 == s2) goto L62c;               // beq v1,s2,0x8013622c
  v0 = (v1 < 2);                         // slti v0,v1,2
  if (v0 == 0) goto L19c;                // beq v0,zero,0x8013619c
  if (v1 == 0) { a0 = (int32_t)nd; goto L1b0; }   // beq v1,zero,0x801361b0 ; delay a0=node
  goto Lret;                             // j 0x801365a8

 L19c:
  v0 = (v1 < 4);                         // slti v0,v1,4
  if (v0 == 0) goto Lret;                // beq v0,zero,0x801365a8
  goto L5a0;                             // j 0x801365a0 (states 2,3)

 L1b0:                                   // ===== STATE 0 (INIT) =====
  // FUN_80051B70(node,12,6)
  c->r[4] = nd; c->r[5] = 12; c->r[6] = 6; eng(c).graphicsBind.recordInit(); v0 = (int32_t)c->r[2];
  if (v0 != 0) goto Lret;                // bne v0,zero,0x801365a8 ; delay v0=576
  v0 = 576;
  a0 = (int32_t)nd;
  c->mem_w16(nd + 0x80, (uint16_t)v0);   // sh v0,128
  v0 = (int32_t)c->mem_r8(nd + 0);       // lbu node[0]
  v1 = 1152;
  c->mem_w8(nd + 0x29, 0);               // sb zero,41
  c->mem_w16(nd + 0x82, (uint16_t)v1);   // sh v1,130
  v0 = v0 | 1;                           // ori
  c->mem_w8(nd + 0, (uint8_t)v0);        // delay sb v0,0
  c->r[4] = nd; rec_dispatch(c, 0x8004766cu);   // FUN_8004766C(node)
  c->r[4] = nd; rec_dispatch(c, 0x80048750u);   // FUN_80048750(node)
  v0 = (int32_t)c->mem_r16(0x1f8001a0u); // lhu scratch[0x1A0]
  v1 = (int32_t)c->mem_r16(nd + 0x2e);   // lhu node[0x2E]
  a0 = (int32_t)c->mem_r16(nd + 0x32);   // lhu node[0x32]
  {
    int32_t a1 = (int32_t)c->mem_r16(nd + 0x36);  // lhu node[0x36]
    c->mem_w16(nd + 0x54, 0);
    c->mem_w16(nd + 0x58, 0);
    c->mem_w16(nd + 0x62, 0);
    c->mem_w8(nd + 4, 1);                // sb s2(=1),4
    c->mem_w8(nd + 5, 0);
    c->mem_w32(nd + 0x68, 0);
    c->mem_w16(nd + 0x56, (uint16_t)v0);
    c->mem_w16(nd + 0x90, (uint16_t)v1);
    c->mem_w16(nd + 0x92, (uint16_t)a0);
    c->mem_w16(nd + 0x94, (uint16_t)a1);
  }
  // fall through to L62c

 L62c:                                   // ===== STATE 1 =====
  v1 = (int32_t)c->mem_r8(nd + 5);       // lbu node[5]
  if (v1 < 0) goto Lret;                 // bltz (dead: lbu>=0) — faithful
  v0 = (v1 < 2);                         // slti v0,v1,2
  if (v0 != 0) goto L258;                // bne v0,zero,0x80136258
  if (v1 == 2) goto L2b4;                // beq v1,2,0x801362b4
  goto Lret;                             // node[5] >= 3

 L258: {                                 // inner A (node[5] 0 or 1) — should-run gate
   v0 = (int32_t)c->mem_r8(0x1f800207u); // lbu scratch[0x207]
   d1 = ((uint32_t)v0 < 24);
   v0 = 1;                               // delay
   if (d1) goto L284;
   v1 = (int32_t)c->mem_r8(0x800bf816u);
   v0 = 1;
   d2 = (v1 != v0);
   v0 = 0;                               // delay
   if (d2) goto L284;
   v0 = 1;
  L284:
   if (v0 != 0) goto Lret;               // bne v0,zero,0x801365a8
   c->mem_w16(nd + 0x44, 0);
   v0 = (int32_t)c->mem_r16(nd + 0x44);  // lhu (=0)
   v1 = (int32_t)c->mem_r32(nd + 0xc0);  // lw 192 (rec)
   c->mem_w16(nd + 0x40, 0);
   c->mem_w16(nd + 0x64, (uint16_t)v0);
   c->mem_w16((uint32_t)v1 + 0x0c, 256);
   c->mem_w8(nd + 5, 2);
   // fall to L2b4
 }

 L2b4: {                                 // inner C (MATH block)
   v0 = (int32_t)c->mem_r8(0x800bf809u); // lbu mem[0x800BF809]
   if (v0 != 0) goto L48c;               // bne v0,zero,0x8013648c
   v1 = c->mem_r16s(nd + 0x44);  // lh node[0x44] (signed)
   uint32_t recp = c->mem_r32(nd + 0xc0);// lw 192
   s2 = (int32_t)c->mem_r16(recp + 0x0c);// lhu rec[0x0C]
   bool lt1281 = (v1 < 1281);
   s1 = v1;                              // delay addu s1,v1,zero
   if (lt1281) goto L314;
   // node[0x44] >= 1281:
   v0 = s1 - 32;
   s1 = v0;
   v1 = (int16_t)(uint16_t)v0;           // sext16
   if (!(v1 < 16385)) { s1 = 16384; goto L34c; }
   if (v1 >= 0) goto L34c;
   s1 = 0;
   goto L34c;
  L314:
   {
     bool ltm1280 = (v1 < -1280);
     v0 = s1 + 32;                       // delay addiu v0,s1,32
     if (!ltm1280) goto L34c;
     s1 = v0;
     v1 = (int16_t)(uint16_t)v0;
     if (v1 < -16384) { s1 = -16384; goto L34c; }
     if (v1 <= 0) goto L34c;
     s1 = 0;
     goto L34c;
   }
  L34c:
   v1 = (int32_t)c->mem_r8(nd + 0x29);   // lbu node[0x29]
   v0 = (int32_t)((uint32_t)s2 << 16);   // delay sll v0,s2,16
   if (v1 != 1) goto L374;
   // node[0x29]==1:
   v0 = (int32_t)((uint32_t)(uint16_t)c->mem_r16(nd + 0x60) << 16) >> 18;  // sext16(node[0x60])>>2
   c->mem_w16(nd + 0x40, 0);
   s1 = s1 + v0;
   goto L38c;
  L374:
   v0 = v0 >> 16;                        // sra: = sext16(s2)
   if (v0 >= 0) goto L384;
   v0 = v0 + 7;
  L384:
   v0 = v0 >> 3;                         // sra
   s1 = s1 - v0;
   // fall to L38c
  L38c:
   {
     int32_t t = (int32_t)((uint32_t)s1 << 16);
     v1 = t >> 24;                       // sra v1,v0,24 = sext16(s1)>>8
     v1 = s2 + v1;
     s2 = v1;
     a0 = t >> 16;                       // sra a0,v0,16 = sext16(s1)
     v1 = (int32_t)((uint32_t)v1 << 16) >> 16;   // sext16(s2)
     bool ltm1024 = (v1 < -1024);
     v0 = (v1 < 1025);                   // delay slti v0,v1,1025
     if (!ltm1024) goto L3e4;            // beq v0,zero(>=-1024),0x801363e4
     // v1 < -1024:
     s2 = -1024;                         // delay 0x801363b8 (runs before bgez decision)
     if (a0 >= 0) goto L41c;
     v0 = -a0;                           // |a0|
     v0 = v0 >> 2;
     s1 = v0;
     a0 = (int32_t)((uint32_t)s2 << 16); // delay 0x801363d8
     if ((int32_t)((uint32_t)s1 << 16) <= 0) goto L420;   // blez
     s1 = 0;
     goto L420;
   }
  L3e4:
   if (v0 != 0) goto L41c;               // bne v0,zero (v1<1025)
   // v1 >= 1025:
   s2 = 1024;                            // delay 0x801363f0
   if (a0 <= 0) goto L41c;               // blez a0
   v0 = -a0;
   v0 = v0 >> 2;                         // arith (negative)
   s1 = v0;
   a0 = (int32_t)((uint32_t)s2 << 16);   // delay 0x80136414
   if ((int32_t)((uint32_t)s1 << 16) >= 0) goto L420;     // bgez
   s1 = 0;
   goto L420;
  L41c:
   a0 = (int32_t)((uint32_t)s2 << 16);   // sll a0,s2,16
  L420:
   s3 = (int32_t)c->mem_r16(nd + 0x36);  // lhu node[0x36]
   a0 = (int32_t)((uint32_t)s2 << 16) >> 16;  // delay sra a0,a0,16 = sext16(s2)
   {
     int32_t sn = trigOf(c).rsin(a0);                        // FUN_80083E80(sin) [native]
     int32_t t = sn << 1; t = t + sn; t = t << 2; t = t + sn;
     t = (int32_t)((uint32_t)t << 6);   // *832
     v0 = (int32_t)c->mem_r16(nd + 0x94);  // lhu node[0x94]
     t = t >> 12;                       // sra
     a0 = v0 + t;
   }
   v1 = (int32_t)c->mem_r8(nd + 0x29);   // lbu node[0x29]
   c->mem_w16(nd + 0x36, (uint16_t)a0);  // delay sh a0,54 (always)
   if (v1 != 1) goto L474;
   // node[0x29]==1: accumulate mem[0x800E7EB6]
   v1 = (int32_t)c->mem_r16(0x800e7eb6u);
   a0 = a0 - s3;
   v1 = v1 + a0;
   c->mem_w16(0x800e7eb6u, (uint16_t)v1);
  L474:
   {
     uint32_t recp = c->mem_r32(nd + 0xc0);
     c->mem_w16(recp + 0x0c, (uint16_t)s2);
   }
   c->mem_w16(nd + 0x44, (uint16_t)s1);
   c->mem_w8(nd + 0x29, 0);
   // fall to L48c
 }

 L48c: {                                 // POST-MATH common — should-run gate again
   v0 = (int32_t)c->mem_r8(0x1f800207u);
   d1 = ((uint32_t)v0 < 24);
   v0 = 1;                               // delay
   if (d1) goto L4b8;
   v1 = (int32_t)c->mem_r8(0x800bf816u);
   v0 = 1;
   d2 = (v1 != v0);
   v0 = 0;                               // delay
   if (d2) goto L4b8;
   v0 = 1;
  L4b8:
   {
     d3 = (v0 == 0);
     v0 = 1;                             // delay 0x801364bc
     if (d3) goto L4c8;
     c->mem_w8(nd + 5, (uint8_t)v0);     // node[5] = 1
     goto L598;
   }
  L4c8:
   v1 = (int32_t)c->mem_r8(0x800e7eaau); // lbu mem[0x800E7EAA]
   {
     bool e31 = (v1 != 31);
     v0 = 34;                            // delay
     if (e31) goto L4ec;
     eng(c).cull.enqueueByClass(nd);            // FUN_8007703C (native)
     goto L4fc;
   }
  L4ec:
   if (v1 == v0) goto L4fc;              // ==34 -> skip
   Actor(c, nd).boundsCull();                       // FUN_8007778C(node) — Actor::boundsCull (native)
  L4fc:
   v0 = (int32_t)c->mem_r8(nd + 1);      // lbu node[1]
   if (v0 == 0) goto L598;
   c->r[4] = nd; eng(c).graphicsBind.renderUpdate();     // FUN_800517F8(node)
   v0 = (int32_t)c->mem_r8(0x800bf809u);
   if (v0 != 0) goto L598;
   v0 = (int16_t)(uint16_t)s1;           // sext16(s1)
   if (v0 >= 0) goto L538;
   v0 = -v0;
  L538:
   if (v0 < 256) {                       // slti 256 ; beq -> >=256 goto L558
     a0 = (int32_t)c->mem_r32(nd + 0x68);// lw 104
     eng(c).areaSlots.ackIfMatch((uint32_t)a0);   // FUN_80074AF0 (native)
     c->mem_w32(nd + 0x68, 0);
     goto L598;
   }
   // >=256 (L558):
   v0 = c->mem_r16s(nd + 0x64);  // lh node[0x64]
   if (v0 >= 0) goto L56c;
   v0 = -v0;
  L56c:
   if (v0 >= 256) goto L598;
   a0 = (int32_t)c->mem_r32(nd + 0x68);
   eng(c).areaSlots.ackIfMatch((uint32_t)a0);   // FUN_80074AF0 (native)
   eng(c).sfx.trigger(129, 0, 0);   // FUN_80074590 (native; id 129 → path A per-area)
   v0 = (int32_t)c->r[2];
   c->mem_w32(nd + 0x68, (uint32_t)v0);
   // fall to L598
 }

 L598:
  c->mem_w16(nd + 0x64, (uint16_t)s1);   // delay sh s1,100
  goto Lret;

 L5a0:
  eng(c).spawn.despawn(nd);                        // FUN_8007A624(node) — native

 Lret:
  return;
}
