// game/ai/beh_prng_velocity_machine.cpp — PC-native per-object BEHAVIOR handler FUN_80117658.
//
// Overlay handler (~x1552/field-frame on seaside; ~430 instr), prologue 0x80117658; `jr ra` at
// 0x80117CF0. Disassembled from scratch/ram/field_seaside.bin. Two-level state machine; outer
// state = node[4] (s0); s2 = node[0x10] (a guest pointer this handler writes through):
//   STATE 0 (init): per-node[3] (0 or 1) variant — set a block of node fields + s2[6/8/10/12/22],
//                   call FUN_80077B38 / FUN_80051B70, then FUN_8004B354 and node[4]++.
//   STATE 1: per node[5]/node[94]/node[3] sub-machine that drives s2[14/20/22/24/26] (velocity/
//            timer fields), uses the PRNG FUN_8009A450 (3 draws), then a shared node[3] dispatch
//            (L7a30): node[3]==0 -> FUN_8007778C + FUN_80077B5C + FUN_8004B374;
//                     node[3]==1 -> reads scratchpad 0x1F800207 gate + node[0xC0] struct -> pos update.
//   STATE 2: per node[3]/node[5] — sound/effect leaves (FUN_8004D4C4/4F4, FUN_8004ED94, FUN_8004B0D8,
//            FUN_8004BD04/BEA8, FUN_80042354, FUN_80040CDC, FUN_8005308C) + area-flag poke 0x800BF9DC|=1.
//   STATE 3: FUN_8007A624(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/s2/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf; a0/a1/a2 set as the
// guest does). The PRNG draws return via c->r[2] and advance the shared RNG in RAM — the gate rolls
// RAM back before rec_super_call so both sides draw the same sequence. Transcribed 1:1 as a register
// machine (locals = guest regs, goto labels = guest addresses); signed (lh/sra) vs unsigned (lhu/lbu)
// preserved exactly. DELAY-SLOT stores before a jal execute BEFORE the callee (mirrored here). The
// byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
#include "inventory.h"       // class Inventory — inv(c).give (FUN_8004D4F4)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80117658u;

static inline uint32_t call0(Core* c, uint32_t fn) { rec_dispatch(c, fn); return c->r[2]; }
static inline uint32_t call1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); return c->r[2]; }
static inline uint32_t call2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn); return c->r[2];
}
static inline uint32_t call3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_prng_velocity_machine(Core* c) {
  uint32_t s1 = c->r[4];                        // s1 = a0 (node)
  uint32_t s0 = c->mem_r8(s1 + 4);              // s0 = node[4] = outer state
  uint32_t s2 = c->mem_r32(s1 + 0x10);          // s2 = node[0x10] (guest pointer)

  if (s0 == 1) goto L7814;                       // STATE 1
  if ((int32_t)s0 < 2) { if (s0 == 0) goto L76b8; goto Lret; }  // state<2 -> only 0
  if (s0 == 2) goto L7ba0;                        // STATE 2
  if (s0 == 3) goto L7cd8;                        // STATE 3
  goto Lret;

 // ---------------------------------------------------------------- STATE 0
 L76b8: {
   uint32_t n3 = c->mem_r8(s1 + 3);
   if (n3 == 0) goto L76d8;
   if (n3 == 1) goto L7788;
   goto Lret;
 }
 L76d8: {                                         // node[3]==0
   c->mem_w8(s1 + 0x0b, 16);
   c->mem_w8(s1 + 0x08, 248);
   c->mem_w8(s1 + 0x47, 0);
   c->mem_w16(s1 + 0x5a, 0);
   c->mem_w8(s1 + 0x0d, 0);
   uint32_t t = c->mem_r32(0x800ecf80u);
   c->mem_w32(s1 + 0x3c, t);                      // node[0x3c] = mem[0x800ecf80] (jal delay slot)
   c->r[4] = s1; c->r[5] = 0x8014c808u; c->r[6] = 1; eng(c).graphicsBind.setGeom();   // FUN_80077B38(node, 0x8014C808, 1) — native (recomp: lui 0x8015 + addiu -14328 = 0x8014C808)
   c->mem_w32(s1 + 0x2c, 0x13d20000u);
   uint16_t v1 = c->mem_r16(s1 + 0x2e);
   c->mem_w32(s1 + 0x30, 0xf7e00000u);
   c->mem_w32(s1 + 0x34, 0x15aa0000u);
   c->mem_w16(s1 + 0x80, 60);
   c->mem_w16(s1 + 0x82, 120);
   c->mem_w16(s1 + 0x84, 80);
   c->mem_w16(s1 + 0x86, 160);
   c->mem_w8(s1 + 0, 4);
   c->mem_w16(s2 + 6, v1);
   uint16_t h = c->mem_r16(s1 + 0x32);
   c->mem_w16(s2 + 8, (uint16_t)(h - 420));
   uint16_t hv = c->mem_r16(s1 + 0x36);
   c->mem_w16(s2 + 22, 30);
   c->mem_w16(s2 + 10, hv);
   c->r[5] = 0;                                   // a1 = 0 for FUN_8004B354
   goto L7804;
 }
 L7788: {                                         // node[3]==1
   c->mem_w8(s1 + 0, 4);
   c->mem_w8(s1 + 0x5e, 0);                       // node[0x5e]=0 (jal delay slot, before callee)
   c->r[4] = s1; c->r[5] = 1; c->r[6] = 0; eng(c).graphicsBind.recordInit(); uint32_t r = c->r[2];  // FUN_80051B70(node, 1, 0)
   if (r != 0) goto Lret;
   c->mem_w32(s1 + 0x2c, 0x28d20000u);
   uint16_t v1 = c->mem_r16(s1 + 0x2e);
   c->mem_w32(s1 + 0x30, 0xf9e80000u);
   c->mem_w32(s1 + 0x34, 0x0f800000u);
   c->mem_w16(s1 + 0x80, 70);
   c->mem_w16(s1 + 0x82, 140);
   c->mem_w16(s1 + 0x84, 140);
   c->mem_w16(s1 + 0x86, 140);
   c->mem_w16(s2 + 6, v1);
   uint16_t h = c->mem_r16(s1 + 0x32);
   c->mem_w16(s2 + 8, (uint16_t)(h - 400));
   uint16_t hv = c->mem_r16(s1 + 0x36);
   c->mem_w16(s2 + 12, 82);
   c->mem_w16(s2 + 10, hv);
   c->r[5] = 1;                                   // a1 = 1 for FUN_8004B354
   goto L7804;
 }
 L7804: {                                         // shared state-0 tail (a1 preset in c->r[5])
   uint32_t a1 = c->r[5];
   uint32_t nv = c->mem_r8(s1 + 4) + 1;
   c->mem_w8(s1 + 4, (uint8_t)nv);                // node[4]++ (jal delay slot, before callee)
   call2(c, s1, a1, 0x8004b354u);                 // FUN_8004B354(node, a1)
   goto Lret;
 }

 // ---------------------------------------------------------------- STATE 1
 L7814: {
   uint32_t n5 = c->mem_r8(s1 + 5);
   if (n5 == 0) goto L7834;
   if (n5 == s0) goto L79ac;                       // s0 == 1
   goto L7a30;
 }
 L7834: {                                          // node[5]==0
   uint32_t n94 = c->mem_r8(s1 + 0x5e);
   if (n94 != s0) goto L7874;                       // s0 == 1
   call3(c, 146, 0, 0, 0x80074590u);               // FUN_80074590(146, 0, 0)
   c->mem_w16(s2 + 24, 36);
   c->mem_w16(s2 + 26, 1408);
   c->mem_w16(s2 + 20, 10240);
   uint32_t n5 = c->mem_r8(s1 + 5);
   c->mem_w8(s1 + 5, (uint8_t)(n5 + 1));
   goto L7a30;
 }
 L7874: {                                          // node[94]!=1
   uint32_t n3 = c->mem_r8(s1 + 3);
   if (n3 != 0) goto L78fc;
   int16_t s20 = c->mem_r16s(s2 + 20);
   if (s20 != 0) goto L7a30;
   uint16_t s22 = (uint16_t)(c->mem_r16(s2 + 22) - 1);
   c->mem_w16(s2 + 22, s22);
   if ((int16_t)s22 >= 0) goto L7a30;
   uint32_t r1 = call0(c, 0x8009a450u);            // PRNG draw 1
   uint32_t r2 = call0(c, 0x8009a450u);            // PRNG draw 2
   uint32_t rs0 = r1 & 3u;
   int32_t v1 = (int32_t)(rs0 * 30u) + ((int32_t)r2 >> 11) + 30;
   c->mem_w16(s2 + 22, (uint16_t)v1);
   uint32_t r3 = call0(c, 0x8009a450u);            // PRNG draw 3
   int32_t v0 = (-(int32_t)((r3 & 7u) + 10u)) << 8;
   c->mem_w16(s2 + 20, (uint16_t)v0);
   goto L7a30;
 }
 L78fc: {                                          // node[3]!=0
   c->mem_w8(s1 + 0x2b, 0);                         // node[0x2b]=0 (jal delay slot, before callee)
   call1(c, s1, 0x801141c8u);                       // FUN_801141C8(node)
   uint32_t n43 = c->mem_r8(s1 + 0x2b);
   if (n43 == 0) goto L7a30;
   uint32_t n3 = c->mem_r8(s1 + 3);
   if (n3 == 0) goto L7934;
   if (n3 == s0) goto L7954;                        // s0 == 1
   goto L7a30;
 }
 L7934: {
   uint32_t n43 = c->mem_r8(s1 + 0x2b);
   if (n43 != 2) { c->mem_w16(s2 + 20, (uint16_t)(int16_t)-8192); goto L7a30; }
   c->mem_w16(s2 + 20, 8192);
   goto L7a30;
 }
 L7954: {
   uint32_t n43 = c->mem_r8(s1 + 0x2b);
   if (n43 != 2) goto L7984;
   int16_t e = c->mem_r16s(s2 + 14);
   uint16_t eu = c->mem_r16(s2 + 14);
   uint16_t nv = (e < 2048) ? (uint16_t)(eu - 4096) : (uint16_t)(-(int32_t)eu);
   c->mem_w16(s2 + 14, nv);
   c->mem_w16(s2 + 20, 3072);
   goto L7a30;
 }
 L7984: {
   int16_t e = c->mem_r16s(s2 + 14);
   uint16_t eu = c->mem_r16(s2 + 14);
   uint16_t nv = (e < -2047) ? (uint16_t)(-(int32_t)eu) : (uint16_t)(eu + 4096);
   c->mem_w16(s2 + 14, nv);
   c->mem_w16(s2 + 20, (uint16_t)(int16_t)-3072);
   goto L7a30;
 }
 L79ac: {                                          // node[5]==1
   int16_t s24 = c->mem_r16s(s2 + 24);
   uint16_t s24u = c->mem_r16(s2 + 24);
   if (s24 == 0) goto L79d0;                         // v0 = -10240
   uint32_t n94 = c->mem_r8(s1 + 0x5e);
   if (n94 != 0) { c->mem_w16(s2 + 24, (uint16_t)(s24u - 1)); goto L7a30; }
   goto L79d0;                                       // v0 = -10240
 }
 L79d0: {
   c->mem_w16(s2 + 20, (uint16_t)(int16_t)-10240);
   c->mem_w16(s2 + 26, 512);
   uint32_t n3 = c->mem_r8(s1 + 3);
   if (n3 == 0) goto L79fc;
   if (n3 == s0) goto L7a04;                         // s0 == 1
   c->mem_w8(s1 + 0, 1);
   goto L7a14;
 }
 L79fc: { c->mem_w16(s1 + 0x36, 5520); goto L7a08b; }
 L7a04: { c->mem_w16(s1 + 0x36, 3968); goto L7a08b; }
 L7a08b: {
   c->mem_w8(s1 + 0, 1);
   goto L7a14;
 }
 L7a14: {
   uint32_t n5 = c->mem_r8(s1 + 5);
   c->mem_w8(s1 + 0x5e, 2);
   c->mem_w8(s1 + 5, (uint8_t)(n5 + 1));
   goto L7a30;
 }

 // ---------------------------------------------------------------- STATE 1 shared movement dispatch
 L7a30: {
   uint32_t n3 = c->mem_r8(s1 + 3);
   if (n3 == 0) goto L7a50;
   if (n3 == 1) goto L7ad4;
   goto Lret;
 }
 L7a50: {                                          // node[3]==0
   uint32_t lp = c->mem_r32(s1 + 0x10);
   uint32_t r = call1(c, s1, 0x8007778cu);          // FUN_8007778C(node)
   if (r == 0) goto Lret;
   uint32_t n5 = c->mem_r8(s1 + 5);
   if (n5 != 2) {                                    // L7a98
     uint16_t a = c->mem_r16(lp + 0);
     c->mem_w16(s1 + 0x2e, (uint16_t)(a - 32));
     uint16_t b = c->mem_r16(lp + 2);
     c->mem_w16(s1 + 0x32, (uint16_t)(b + 80));
     goto L7ab8;
   }
   c->mem_w16(s1 + 0x2e, c->mem_r16(0x1f800160u));
   c->mem_w16(s1 + 0x32, c->mem_r16(0x1f800162u));
   c->mem_w16(s1 + 0x36, c->mem_r16(0x1f800164u));
   goto L7ab8;
 }
 L7ab8: {
   call1(c, s1, 0x80077b5cu);                        // FUN_80077B5C(node)
   call2(c, s1, 0, 0x8004b374u);                     // FUN_8004B374(node, 0)
   goto Lret;
 }
 L7ad4: {                                          // node[3]==1
   uint32_t sp207 = c->mem_r8(0x1f800207u);
   uint32_t lp = c->mem_r32(s1 + 0x10);
   if (!(sp207 < 5)) goto Lret;
   uint32_t r = call1(c, s1, 0x8007778cu);          // FUN_8007778C(node)
   if (r == 0) goto Lret;
   uint32_t n5 = c->mem_r8(s1 + 5);
   if (n5 != 2) {                                    // L7b2c
     uint32_t p = c->mem_r32(s1 + 0xc0);
     int16_t e = c->mem_r16s(p + 26);
     int32_t v1 = ((int32_t)e * 9) >> 8;
     uint16_t a = c->mem_r16(lp + 0);
     c->mem_w16(s1 + 0x2e, (uint16_t)(a + v1));
     int16_t f = c->mem_r16s(p + 32);
     int32_t v0 = ((int32_t)f * 9) >> 8;
     uint16_t b = c->mem_r16(lp + 2);
     c->mem_w16(s1 + 0x32, (uint16_t)(b + v0));
     uint16_t g = c->mem_r16(lp + 12);
     c->mem_w16(s1 + 0x58, (uint16_t)(-(int32_t)g));
     goto L7b84;
   }
   c->mem_w16(s1 + 0x2e, c->mem_r16(0x1f800160u));
   c->mem_w16(s1 + 0x32, c->mem_r16(0x1f800162u));
   c->mem_w16(s1 + 0x36, c->mem_r16(0x1f800164u));
   goto L7b84;
 }
 L7b84: {
   c->r[4] = s1; eng(c).graphicsBind.renderUpdate();            // FUN_800517F8(node) — native
   call2(c, s1, 1, 0x8004b374u);                     // FUN_8004B374(node, 1)
   goto Lret;
 }

 // ---------------------------------------------------------------- STATE 2
 L7ba0: {
   uint32_t n3 = c->mem_r8(s1 + 3);
   if (n3 == 0) goto L7bc0;
   if (n3 == 1) goto L7bd8;
   goto Lret;
 }
 L7bc0: {                                          // node[3]==0
   inv(c).giveAndFlag(58, 1);                  // FUN_8004D4C4(58, 1) [native]
   call1(c, s1, 0x8004b0d8u);                        // FUN_8004B0D8(node)
   c->mem_w8(s1 + 4, 3);                             // node[4]=3
   goto Lret;
 }
 L7bd8: {                                          // node[3]==1
   uint32_t n5 = c->mem_r8(s1 + 5);
   if (n5 == 3) goto L7c7c;
   if ((int32_t)n5 < 2) { if (n5 == 0) goto L7c10; goto Lret; }
   goto L7c00;
 }
 L7c00: {
   uint32_t n5 = c->mem_r8(s1 + 5);
   if (n5 == s0) goto L7cb0;                         // s0 == 2
   goto Lret;
 }
 L7cb0: {
   uint32_t n112 = c->mem_r8(s1 + 0x70);
   if (n112 != 255) goto L7cc8;
   c->mem_w8(s1 + 4, 3);                             // node[4]=3 (fall-through L7cc0)
   goto Lret;
 }
 L7cc8: {
   call1(c, s1, 0x80041098u);                        // FUN_80041098(node)
   goto Lret;
 }
 L7c10: {                                          // node[5]==0
   inv(c).give(40, 1);                         // FUN_8004D4F4(40, 1) [native]
   call2(c, 45, 66, 0x8004ed94u);                    // FUN_8004ED94(45, 66)
   call1(c, s1, 0x8004b0d8u);                        // FUN_8004B0D8(node)
   uint32_t r = call3(c, s1, 0, 0, 0x8004bd04u);     // FUN_8004BD04(node, 0, 0)
   if (r == 0) goto L7c70;
   call2(c, 40, r, 0x8004bea8u);                     // FUN_8004BEA8(40, r)
   call2(c, 1, 1, 0x80042354u);                      // FUN_80042354(1, 1)
   uint8_t fl = c->mem_r8(0x800bf9dcu);
   c->mem_w8(0x800bf9dcu, (uint8_t)(fl | 1));        // area flag |= 1
   goto L7c70;
 }
 L7c70: {
   uint32_t n5 = c->mem_r8(s1 + 5);
   c->mem_w8(s1 + 5, (uint8_t)(n5 + 1));
   goto Lret;
 }
 L7c7c: {                                          // node[5]==3
   uint32_t r = call0(c, 0x8005308cu);               // FUN_8005308C()
   if (r == 0) goto Lret;
   call3(c, s1, 0, 0x80148574u, 0x80040cdcu);        // FUN_80040CDC(node, 0, 0x80148574) — recomp: lui 0x8015 + addiu -31372 = 0x80148574
   uint32_t n5 = c->mem_r8(s1 + 5);
   c->mem_w8(s1 + 0x70, (uint8_t)s0);                // node[0x70] = s0 (== 2)
   c->mem_w8(s1 + 5, (uint8_t)(n5 + 1));
   goto Lret;
 }

 // ---------------------------------------------------------------- STATE 3
 L7cd8: {
   eng(c).spawn.despawn(s1);                             // FUN_8007A624(node) — native
   goto Lret;
 }

 Lret:
  return;
}
