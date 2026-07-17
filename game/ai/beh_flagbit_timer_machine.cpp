// game/ai/beh_flagbit_timer_machine.cpp — PC-native per-object BEHAVIOR handler FUN_8013B2E4.
//
// Overlay handler (~x778/field-frame on seaside; ~150 instr), prologue 0x8013B2E4; `jr ra` at
// 0x8013B52C. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4] (s0):
//   STATE 0 : INIT. FUN_800519E0(node, 3, mem[0x800ECFD4], 0x8015AABC); if !=0 return. node[0x3C]=
//             mem[0x800ECFD8]; if global mem[0x800BF873]!=0 -> node[4]=3 return. Test bit node[3] of
//             mem[0x800BFA13]: set -> node[0x5E]=0, a2=0; clear -> FUN_8013AF18(node,1,31), a2=1; then
//             FUN_80077C40(node, 0x8001B7B0, a2). Seed node[0]=1, node[0x80..0x86]=400/800/180/180,
//             node[0x29]=node[0x2B]=0, node[4]++; if node[0x5E]==0 also node[5]=4.
//   STATE 1 : FUN_8007778C(node) (0 -> tail); FUN_80076D68(node); per node[6] (0/1/2/>2):
//             ==1 -> node[0x40]-- and when it hits 0 set node[0]=2,node[6]++; ==2 -> node[0]=1,node[6]=0,
//             clear global 0x800BF809, then fall to the node[0x2B]==3 block (node[0x40]=30, node[6]++,
//             FUN_8004ED94(97|98,65), set 0x800BF809=1); ==0 -> that same block; then FUN_8013B024(node,31)
//             + FUN_800518FC(node). Tail clears node[0x29]=node[0x2B]=0.
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/global WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf; all args set as the guest does).
// srav shift masked to (node[3]&31). Transcribed 1:1 as a register machine (goto labels = guest addresses);
// delay-slot stores before a jal mirrored to run before the callee. The byte-exact A/B gate (full RAM+
// scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "animation.h" // Animation::step (FUN_80076D68)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013B2E4u;

static inline uint32_t leafr(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); return c->r[2]; }
static inline uint32_t leaf4r(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_flagbit_timer_machine(Core* c) {
  uint32_t nd = c->r[4];                          // s1 = a0 (node)
  uint32_t s0 = c->mem_r8(nd + 4);                // node[4] = outer state
  if (getenv("PSXPORT_FLAGBIT_ENTRY"))
    fprintf(stderr, "[flagbit-entry] core=%p node=%08X s0=%u stage=%08X\n",
            (void*)c, nd, s0, c->mem_r32(0x801fe00c));

  if (s0 == 1) goto L42c;
  if ((int32_t)s0 < 2) { if (s0 == 0) goto S0; goto Lret; }
  if (s0 == 2) goto Lret;
  if (s0 == 3) { eng(c).spawn.despawn(nd); goto Lret; }
  goto Lret;

 // ================= STATE 0 (INIT) =================
 S0: {
   uint32_t a2v = c->mem_r32(0x800ecfd4u);
   if (getenv("PSXPORT_FLAGBIT_TRACE"))
     fprintf(stderr, "[flagbit] core=%p S0 node=%08X cnt_before=%d stage=%08X\n",
             (void*)c, nd, (int)c->mem_r16s(0x800ED098u), c->mem_r32(0x801fe00c));
   if (leaf4r(c, nd, 3, a2v, 0x8014aabcu, 0x800519e0u) != 0) goto Lret;   // FUN_800519E0 (Slip #6 fix: was 0x8015AABC — 0x1000 hex-typo, verified against recomp ov_a00_gen_8013B2E4 line 24959 which computes r[7] = (0x8015<<16) + (-21828) = 0x8014AABC)
   c->mem_w32(nd + 0x3c, c->mem_r32(0x800ecfd8u));
   if (c->mem_r8(0x800bf873u) != 0) { c->mem_w8(nd + 4, 3); goto Lret; }
   uint32_t bit = (c->mem_r8(0x800bfa13u) >> (c->mem_r8(nd + 3) & 31)) & 1u;
   uint32_t a2c;
   if (bit != 0) { c->mem_w8(nd + 0x5e, 0); a2c = 0; }
   else { guest_leaf(c, 0x8013af18u, nd, 1, 31); a2c = 1; }                     // FUN_8013AF18(node,1,31)
   guest_leaf(c, 0x80077c40u, nd, 0x8001b7b0u, a2c);                            // FUN_80077C40(node,0x8001B7B0,a2)
   c->mem_w8(nd + 0, 1);
   c->mem_w16(nd + 0x80, 400);
   c->mem_w16(nd + 0x82, 800);
   c->mem_w16(nd + 0x84, 180);
   c->mem_w16(nd + 0x86, 180);
   uint32_t nv = c->mem_r8(nd + 4) + 1;
   uint8_t n5e = c->mem_r8(nd + 0x5e);
   c->mem_w8(nd + 0x29, 0);
   c->mem_w8(nd + 0x2b, 0);
   c->mem_w8(nd + 4, (uint8_t)nv);
   if (n5e != 0) goto Lret;
   c->mem_w8(nd + 5, 4);
   goto Lret;
 }

 // ================= STATE 1 =================
 L42c: {
   if (leafr(c, nd, 0x8007778cu) == 0) goto L50c;   // FUN_8007778C
   eng(c).animation.step(nd);                       // FUN_80076D68 (native)
   uint8_t n6 = c->mem_r8(nd + 6);
   if (n6 == s0) goto L4cc;                            // s0 == 1
   if ((int32_t)n6 < 2) { if (n6 == 0) goto L484; goto L4f8; }
   // n6 >= 2
   if (n6 != 2) goto L4f8;
   c->mem_w8(nd + 0, (uint8_t)s0);                     // node[0] = 1
   c->mem_w8(nd + 6, 0);
   c->mem_w8(0x800bf809u, 0);
   goto L484;
 }
 L484: {
   if (c->mem_r8(nd + 0x2b) != 3) goto L4f8;
   c->mem_w16(nd + 0x40, 30);
   uint8_t n6 = c->mem_r8(nd + 6);
   uint8_t n5e = c->mem_r8(nd + 0x5e);
   c->mem_w8(nd + 6, (uint8_t)(n6 + 1));
   uint32_t a0 = (n5e != 0) ? 97u : 98u;
   guest_leaf(c, 0x8004ed94u, a0, 65);                     // FUN_8004ED94(97|98, 65)
   c->mem_w8(0x800bf809u, 1);
   goto L4f8;
 }
 L4cc: {
   uint16_t v = (uint16_t)(c->mem_r16(nd + 0x40) - 1);
   c->mem_w16(nd + 0x40, v);
   if (v != 0) goto L4f8;
   uint8_t n6 = c->mem_r8(nd + 6);
   c->mem_w8(nd + 0, 2);
   c->mem_w8(nd + 6, (uint8_t)(n6 + 1));
   goto L4f8;
 }
 L4f8: {
   guest_leaf(c, 0x8013b024u, nd, 31);                     // FUN_8013B024(node, 31)
   guest_leaf(c, 0x800518fcu, nd);                          // FUN_800518FC(node)
   goto L50c;
 }
 L50c: {
   c->mem_w8(nd + 0x29, 0);
   c->mem_w8(nd + 0x2b, 0);
   goto Lret;
 }

 Lret:
  return;
}
