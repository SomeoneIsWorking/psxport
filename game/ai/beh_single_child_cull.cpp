// game/ai/beh_single_child_cull.cpp — PC-native per-object BEHAVIOR handler FUN_80132400.
//
// Overlay handler (~x778/field-frame on seaside; ~80 instr), prologue 0x80132400; `jr ra` at
// 0x80132540. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT. v0 = FUN_80051B70(node, 12, 37); if v0!=0 -> return. Else seed node[0x80..0x86]=
//             30/60/50/100, node[0x60]=-2350, node[0x62]=-1630, node[0x50]=1920, node[0]=1,
//             node[0x29]=0, node[0x5E]=0, node[4]++, node[0x32]+=128, node[3]=0; then
//             node[0x10] = FUN_8013A730(node).
//   STATE 1 : if mem[0x800BF89C]==2 OR mem[0x800E7EAA]!=node[4]: v0=FUN_8007778C(node); if v0!=0 ->
//             FUN_80132020(node) + FUN_800517F8(node). ALWAYS node[0x2B]=0 at the tail.
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). a0 fidelity: the
// guest sets a0=node before the STATE-0 FUN_80051B70 (delay-slot `addu a0,s0,zero`) and again before
// FUN_8013A730; in STATE 1, a0 is still the original node[4] byte (compared vs mem[0x800E7EAA]). The
// dead `addiu v1,v0,-1936` (= 0x800BF870, never stored/read) is dropped — no RAM effect. Transcribed
// 1:1 as a register machine; signed (sh) preserved. The byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. NO GTE.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (c->engine.spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80132400u;

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}
static inline uint32_t leafr3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_single_child_cull(Core* c) {
  uint32_t nd = c->r[4];                          // s0 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                // a0 = node[4] = outer state

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) { c->engine.spawn.despawn(nd); goto Lret; }
  goto Lret;

 // ================= STATE 0 (INIT) =================
 S0: {
   c->r[4] = nd; c->r[5] = 12; c->r[6] = 37; c->engine.graphicsBind.recordInit();   // FUN_80051B70(node,12,37) -> bail if !=0
   if (c->r[2] != 0) goto Lret;
   c->mem_w16(nd + 0x80, 30);
   c->mem_w16(nd + 0x82, 60);
   c->mem_w16(nd + 0x84, 50);
   c->mem_w16(nd + 0x86, 100);
   c->mem_w16(nd + 0x60, (uint16_t)(int16_t)-2350);
   c->mem_w16(nd + 0x62, (uint16_t)(int16_t)-1630);
   c->mem_w16(nd + 0x50, 1920);
   uint32_t s = c->mem_r8(nd + 4);                 // node[4]
   uint32_t h = c->mem_r16(nd + 0x32);             // node[0x32]
   c->mem_w8(nd + 0, 1);
   c->mem_w8(nd + 0x29, 0);
   c->mem_w8(nd + 0x5e, 0);
   c->mem_w8(nd + 4, (uint8_t)(s + 1));            // node[4]++
   c->mem_w16(nd + 0x32, (uint16_t)(h + 128));     // node[0x32] += 128
   c->mem_w8(nd + 3, 0);
   c->mem_w32(nd + 0x10, leafr1(c, nd, 0x8013a730u));   // node[0x10] = FUN_8013A730(node)
   goto Lret;
 }

 // ================= STATE 1 =================
 S1: {
   bool work;
   if (c->mem_r8(0x800bf89cu) == 2) work = true;
   else work = (c->mem_r8(0x800e7eaau) != st);     // st = original node[4] byte
   if (work) {
     if (leafr1(c, nd, 0x8007778cu) != 0) {        // FUN_8007778C(node)
       leaf1(c, nd, 0x80132020u);                  // FUN_80132020(node)
       c->r[4] = nd; c->engine.graphicsBind.renderUpdate();                  // FUN_800517F8(node)
     }
   }
   c->mem_w8(nd + 0x2b, 0);                         // node[0x2B] = 0 (every STATE-1 path)
   goto Lret;
 }

 Lret:
  return;
}
