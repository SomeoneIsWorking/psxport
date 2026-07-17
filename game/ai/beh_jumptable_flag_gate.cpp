// game/ai/beh_jumptable_flag_gate.cpp — PC-native per-object BEHAVIOR handler FUN_8012D4EC (OVERLAY).
//
// An OVERLAY-resident per-object behavior routine (lives in the field overlay, not MAIN.EXE), installed
// at node+0x1c and called every frame by the entity walk with the object node in a0. Same SHAPE as the
// resident siblings (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler): a state machine on the node's state
// byte node[4] (0 init / 1 active / 2 idle / 3 despawn). State 0 seeds box/size via FUN_80051b70 then
// (depending on node[3] and DAT_800bf9df) sets node[4]=3 or node[0xb] and runs FUN_800517f8. State 1 is a
// 6-way sub-machine dispatched by JUMP TABLE 0x80109D94 on node[3] (the object type, in [0,5]); state 2
// returns (idle); state 3 despawns via FUN_8007A624.
//
// node[3] jump-table targets (RE'd from the field RAM dump at 0x80109D94):
//   jt[0]=0x8012d62c  jt[1]=0x8012d6f0  jt[2]=0x8012d85c  jt[3]=0x8012d85c
//   jt[4]=0x8012d87c  jt[5]=0x8012d89c                 (jt[2]==jt[3]: shared "gate-then-flag" block)
//
// Globals referenced (computed from the lui/addiu pairs in the disasm):
//   0x800E7E80 (a1 base; lh 0x36(a1) -> 0x800E7EB6, a counter checked < 0x2a30 when 0x1F800207==0x16)
//   0x800BF9DF (== base 0x800BF870 + 0x16f; lbu gate byte, &2 advances node[5]; also bit-flagged 4/8 on despawn)
//   0x1F800207 (scratchpad lbu; a global progress/phase counter compared against thresholds)
//   0x800BF80D (lb signed; state-1 entry gate; !=0 -> node[1]=1 + tail)
//   0x1F8000DE (scratchpad lh; jt[1] node[5]==0 second gate, < 0x3f8c)
//   0x800BF9B5 (== base 0x800BF870 + 0x145; jt[5] gate byte, ==3 -> node[1]=1)
//
// Ownership model (identical to the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler): CONTROL FLOW + node/global
// memory writes owned native; every sub-behavior CALL stays reachable by address via rec_dispatch (NO
// recursion into them). NO GTE, NO render packets here. RE'd 1:1 from disas 0x8012D4EC..0x8012D904
// (overlay), JT 0x80109D94. It WRITES guest node state the still-recomp content reads -> content-INTERFACE:
// gated byte-exact (full RAM+scratchpad A/B vs rec_super_call). Most sub-states are scene/progress driven
// and only exercise when a scene drives them (same caveat as the sibling orchestrators) — see Report.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_record_init
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012D4ECu;

// ---- shared tail blocks reached by the state-1 sub-machine ----------------------------------------

// LAB_8012d844: v0=1; fall into 8012d848 (node[1]=1); then 8012d84c (jal 0x800517f8); j epilogue.
// LAB_8012d848: node[1]=v0; jal 0x800517f8(a0=obj); j epilogue.  (caller supplies v0; here always 1)
inline void tail_set1_and_render(Core* c, uint32_t obj) {  // 0x8012d844 / 0x8012d848 / 0x8012d84c
  c->mem_w8(obj + 1, 1);                                    // 8012D848 sb v0(=1),1(s0)
  c->r[4] = obj; eng(c).graphicsBind.renderUpdate();             // 8012D84C jal 0x800517f8 ; 8012D850 move a0,s0
}

// LAB_8012d82c..8012d840: set bit (4 if node[3]==0 else 8) in DAT_800bf9df; node[0x18..0x1a]=0; node[4]=3.
// Reached from the "step_node18 reached <=0" path. v1 = 0x800BF870 throughout (see disasm note below).
inline void despawn_flag_block(Core* c, uint32_t obj) {     // 0x8012d7fc..8012d840 (and the 8012d6d0 inline copy)
  uint8_t n3 = c->mem_r8(obj + 3);                          // 8012D7FC/8012D6D0 lbu v0,3(s0)
  uint8_t cur = c->mem_r8(0x800BF9DFu);                     // 8012D810/8012D820 lbu v0,0x16f(v1)
  // bnez v0 -> 0x8012d81c (ori 8) ; fall-through (node[3]==0) -> ori 4 (0x8012d810/8012d6e4)
  uint8_t nv = (n3 != 0) ? (uint8_t)(cur | 8) : (uint8_t)(cur | 4);
  c->mem_w8(0x800BF9DFu, nv);                               // 8012D82C sb v0,0x16f(v1)
  c->mem_w8(obj + 0x1a, 0);                                 // 8012D834 sb zero,0x1a(s0)
  c->mem_w8(obj + 0x19, 0);                                 // 8012D838 sb zero,0x19(s0)
  c->mem_w8(obj + 0x18, 0);                                 // 8012D83C sb zero,0x18(s0)
  c->mem_w8(obj + 4, 3);                                    // 8012D840 sb v0(=3),4(s0)  -> state 3
}

// LAB_8012d7d8 (and its inline copy at 0x8012d6ac): node[0x18,0x19,0x1a] = node[0x18]-3; if (int8)result>0
// -> tail_set1_and_render; else despawn_flag_block then tail_set1_and_render.  Returns true if it took the
// ">0" early exit (caller must still run tail_set1_and_render in both cases — see callers).
inline void step_node18(Core* c, uint32_t obj) {            // 0x8012d7d8 / inline 0x8012d6ac
  uint8_t v = (uint8_t)(c->mem_r8(obj + 0x18) - 3);         // 8012D7D8/6AC lbu v0,0x18 ; addiu v0,-3
  c->mem_w8(obj + 0x1a, v);                                 // 8012D7E4/6B8 sb v0,0x1a(s0)
  c->mem_w8(obj + 0x19, v);                                 // 8012D7E8/6BC sb v0,0x19(s0)
  c->mem_w8(obj + 0x18, v);                                 // 8012D7EC/6C0 sb v0,0x18(s0)
  // sll v0,0x18 ; bgtz -> 0x8012d848 (skip despawn_flag) when (int8)v > 0
  if ((int8_t)v <= 0) despawn_flag_block(c, obj);           // 8012D7F4/6C8 bgtz; else despawn_flag_block
  tail_set1_and_render(c, obj);                             // -> 0x8012d848 (node[1]=1) then 0x8012d84c
}

// LAB_8012d78c: node[0x32]+=4; if (int16)node[0x32] < -0x64e -> tail_set1_and_render; else
// node[0x18..0x1a]=0x7f, node[0xd]=2, node[6]++, tail_set1_and_render (v0=1 -> node[1]=1).
inline void advance_node32(Core* c, uint32_t obj) {         // 0x8012d78c
  uint16_t v = (uint16_t)(c->mem_r16(obj + 0x32) + 4);      // 8012D78C lhu v0,0x32 ; addiu v0,4
  c->mem_w16(obj + 0x32, v);                                // 8012D798 sh v0,0x32(s0)
  // sll/sra 0x10 (sign-extend) ; slti v0,-0x64e ; bnez -> 0x8012d848 (when (int16)v < -0x64e)
  if ((int16_t)v < -0x64e) { tail_set1_and_render(c, obj); return; }  // 8012D7A8 bnez -> 0x8012d848
  c->mem_w8 (obj + 0x1a, 0x7f);                             // 8012D7B4 sb 0x7f,0x1a(s0)
  c->mem_w8 (obj + 0x19, 0x7f);                             // 8012D7B8 sb 0x7f,0x19(s0)
  c->mem_w8 (obj + 0x18, 0x7f);                             // 8012D7BC sb 0x7f,0x18(s0)
  c->mem_w8 (obj + 0xd, 2);                                 // 8012D7C8 sb v1(=2),0xd(s0)
  c->mem_w8 (obj + 6, (uint8_t)(c->mem_r8(obj + 6) + 1));   // 8012D7CC/7D4 lbu/addiu/sb node[6]++
  tail_set1_and_render(c, obj);                             // 8012D7D0 j 0x8012d844 -> node[1]=1, render
}

// LAB_8012d85c (jt[2]/jt[3]) and 8012d87c (jt[4]): "gate-then-flag" — if DAT_800bf9df != 0 -> node[1]=1;
// else if (0x1F800207) >= thr -> node[1]=1; else epilogue. (thr=0x1f for jt2/3, 0x20 for jt4.)
inline void gate_then_flag(Core* c, uint32_t obj, uint32_t thr) {
  if (c->mem_r8(0x800BF9DFu) != 0) {                        // 8012D860/880 lbu -0x621 ; bnez -> 0x8012d8e0
    c->mem_w8(obj + 1, 1);                                  // 8012D8E0/E4 v0=1 ; 8012D8E8 sb v0,1(s0)
    return;                                                 // -> epilogue
  }
  uint8_t p = c->mem_r8(0x1F800207u);                       // 8012D870/890 lbu 0x207(1f80)
  // sltiu v0,p,thr ; (j 0x8012d8d8) bnez v0 -> epilogue (when p < thr) ; else node[1]=1
  if (p < thr) return;                                      // 8012D8D8 bnez -> 0x8012d8f4 epilogue
  c->mem_w8(obj + 1, 1);                                    // 8012D8E0/E4/E8 node[1]=1
}

}  // namespace

void beh_jumptable_flag_gate(Core* c) {
  const uint32_t obj = c->r[4];                             // 8012D4F4 move s0,a0
  uint8_t st = c->mem_r8(obj + 4);                          // 8012D504 lbu v1,4(s0)  (state byte)
  // a1 (= 0x800E7E80) is set in the prologue delay slot (8012D510) but only read on the state-1 path.

  // ---- state dispatch [0x8012D50C..0x8012D548] ----
  if (st != 1) {                                            // 8012D50C beq v1,s1(=1) -> state 1
    if (st >= 2) {                                          // 8012D514 slti v0,v1,2 ; 8012D518 beqz -> 0x8012d530
      if (st == 2) return;                                  // 8012D534 beq v1,2 -> 0x8012d8f4 (epilogue)
      if (st == 3) {                                        // 8012D53C beq v1,3 -> 0x8012d8ec (despawn)
        eng(c).spawn.despawn(obj);        // 8012D8EC jal 0x8007a624 ; 8012D8F0 move a0,s0
      }
      return;                                               // 8012D544 j 0x8012d8f4 (epilogue; other >=2 = no-op)
    }
    if (st != 0) return;                                    // 8012D520 beqz v1 -> state 0; else 8012D528 j epilogue

    // ---- STATE 0 [0x8012D54C]: box/size init ----
    uint8_t n3 = c->mem_r8(obj + 3);                        // 8012D54C lbu a2,3(s0)
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = (uint32_t)n3 + 0x48;  // 8012D550 a1=0xc ; 8012D558 a2=node[3]+0x48
    eng(c).graphicsBind.recordInit();                           // 8012D554 jal 0x80051b70
    if (c->r[2] != 0) return;                               // 8012D55C bnez v0 -> 0x8012d8f4 (init busy -> epilogue)
    n3 = c->mem_r8(obj + 3);                                // 8012D568 lbu v1,3(s0)
    c->mem_w8(obj + 0, 1);                                  // 8012D56C sb s1(=1),0(s0)  node[0]=1
    c->mem_w8(obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));  // 8012D564/570/57C node[4]++ (-> state 1)
    // sltiu v1,node[3],2 ; beqz v1 -> 0x8012d5a4  (taken when node[3] >= 2)
    if (n3 >= 2) {                                          // 8012D578 beqz -> 0x8012d5a4
      c->mem_w8(obj + 0xb, 2);                              // 8012D5AC sb v0(=2),0xb(s0)
      c->r[4] = obj; eng(c).graphicsBind.renderUpdate();          // 8012D84C jal 0x800517f8 (via j 0x8012d84c)
      return;                                               // 8012D854 j epilogue
    }
    // node[3] < 2:
    if (c->mem_r8(0x800BF9DFu) != 0) {                      // 8012D584 lbu -0x621 ; 8012D58C beqz -> 0x8012d59c
      c->mem_w8(obj + 4, 3);                                // 8012D598 sb v0(=3),4(s0)  -> state 3
      return;                                               // 8012D594 j 0x8012d8f4 (epilogue)
    }
    c->mem_w8(obj + 0xb, 0);                                // 8012D5A0 sb zero,0xb(s0)  (delay slot of j 0x8012d84c)
    c->r[4] = obj; eng(c).graphicsBind.renderUpdate();            // 8012D84C jal 0x800517f8
    return;                                                 // 8012D854 j epilogue
  }

  // ============================================================================================
  // STATE 1 [0x8012D5B0]: entry gates, then node[3] jump-table sub-machine
  // ============================================================================================
  uint8_t p0 = c->mem_r8(0x1F800207u);                      // 8012D5B4 lbu a0,0x207(1f80)
  if (p0 < 0x16) return;                                    // 8012D5C0 bnez(p0<0x16) -> 0x8012d8f4 epilogue
  if (p0 == 0x16) {                                         // 8012D5C8 bne a0,0x16 -> 0x8012d5e4 (skip when !=)
    int16_t cnt = c->mem_r16s(0x800E7EB6u);         // 8012D5D0 lh v0,0x36(a1)  (a1=0x800e7e80)
    if (cnt < 0x2a30) return;                               // 8012D5DC bnez(cnt<0x2a30) -> 0x8012d8f4 epilogue
  }
  // [0x8012D5E4] entry gate on DAT_800bf80d
  if (c->mem_r8s(0x800BF80Du) != 0) {                // 8012D5E4 lb v0 (signed) ; 8012D5EC beqz -> 0x8012d5fc
    c->mem_w8(obj + 1, 1);                                  // 8012D5F8 sb v1(=node[4]=1),1(s0)  node[1]=1
    return;                                                 // 8012D594... j 0x8012d8f4 epilogue
  }

  uint8_t n3 = c->mem_r8(obj + 3);                          // 8012D5FC lbu v1,3(s0)
  if (n3 >= 6) return;                                      // 8012D608 beqz(sltiu n3,6) -> 0x8012d8f4 epilogue
  // jump table 0x80109D94, node[3]*4 -> {d62c,d6f0,d85c,d85c,d87c,d89c}
  switch (n3) {

  case 0: {  // jt[0] = 0x8012d62c
    uint8_t s5 = c->mem_r8(obj + 5);                        // 8012D62C lbu v1,5(s0)
    if (s5 == 0) {                                          // 8012D634 beqz -> 0x8012d64c (delay v0=1)
      // ---- 0x8012d64c: node[5]==0 ----
      if (c->mem_r8(0x800BF9DFu) & 2) {                     // 8012D650 lbu -0x621 ; 8012D658 andi 2 ; 8012D65C beqz
        c->mem_w8(obj + 6, 0);                              // 8012D668 sb zero,6(s0)
        c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));  // 8012D664/66C/670 node[5]++
      }
      // 0x8012d678:
      if (c->mem_r8(0x1F800207u) < 0x17) return;           // 8012D684 bnez(p<0x17) -> 0x8012d8f4 epilogue
      c->mem_w8(obj + 1, 1);                                // 8012D690 sb v0(=1),1(s0)  (j 0x8012d8f4 delay slot)
      return;                                               // 8012D68C j epilogue
    }
    if (s5 != 1) return;                                    // 8012D63C beq v1,1 -> 0x8012d694 ; else j epilogue
    // ---- 0x8012d694: node[5]==1 ----
    uint8_t n6 = c->mem_r8(obj + 6);                        // 8012D694 lbu v0,6(s0)
    if (n6 == 0) { advance_node32(c, obj); return; }        // 8012D69C beqz -> 0x8012d78c
    if (n6 != 1) { tail_set1_and_render(c, obj); return; }  // 8012D6A4 bne v0,v1(=1) -> 0x8012d848 (node[1]=1)
    // n6 == 1: inline step_node18 (0x8012d6ac == 0x8012d7d8 logic)
    step_node18(c, obj);
    return;
  }

  case 1: {  // jt[1] = 0x8012d6f0
    uint8_t s5 = c->mem_r8(obj + 5);                        // 8012D6F0 lbu v1,5(s0)
    if (s5 == 0) {                                          // 8012D6F8 beqz -> 0x8012d710 (delay v0=1)
      // ---- 0x8012d710: node[5]==0 ----
      if (c->mem_r8(0x800BF9DFu) & 2) {                     // 8012D714 lbu -0x621 ; 8012D71C andi 2 ; 8012D720 beqz
        c->mem_w8(obj + 6, 0);                              // 8012D72C sb zero,6(s0)
        c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));  // 8012D728/730/734 node[5]++
      }
      // 0x8012d73c:
      if (c->mem_r8(0x1F800207u) < 0x1c) return;           // 8012D748 bnez(p<0x1c) -> 0x8012d8f4 epilogue
      if (c->mem_r16s(0x1F8000DEu) < 0x3f8c) return;  // 8012D758/75C bnez(lh<0x3f8c) -> 0x8012d8f4 epilogue
      c->mem_w8(obj + 1, 1);                                // 8012D768 sb v0(=1),1(s0)  (j 0x8012d8f4 delay slot)
      return;                                               // 8012D764 j epilogue
    }
    if (s5 != 1) return;                                    // 8012D700 beq v1,1 -> 0x8012d76c ; else j epilogue
    // ---- 0x8012d76c: node[5]==1 ----
    uint8_t n6 = c->mem_r8(obj + 6);                        // 8012D76C lbu v0,6(s0)
    if (n6 == 0) { advance_node32(c, obj); return; }        // 8012D774 beqz -> 0x8012d78c
    if (n6 == 1) { step_node18(c, obj); return; }           // 8012D77C beq v0,v1(=1) -> 0x8012d7d8
    // n6 != 0 && != 1: node[1]=1 then render (j 0x8012d84c, node[1]=1 in delay slot)
    tail_set1_and_render(c, obj);                           // 8012D788 sb v0(=1),1(s0) ; 8012D784 j 0x8012d84c
    return;
  }

  case 2:
  case 3:  // jt[2]==jt[3] = 0x8012d85c
    gate_then_flag(c, obj, 0x1f);                           // 8012D85C..  thr=0x1f (sltiu p,0x1f)
    return;

  case 4:  // jt[4] = 0x8012d87c
    gate_then_flag(c, obj, 0x20);                           // 8012D87C..  thr=0x20 (sltiu p,0x20)
    return;

  case 5: {  // jt[5] = 0x8012d89c
    if (c->mem_r8(0x800BF9B5u) == 3) {                      // 8012D8A4 lbu 0x145(a0=0x800bf870) ; 8012D8AC beq v1,3
      c->mem_w8(obj + 1, 1);                                // 8012D8E4/E8 v0=1 ; sb v0,1(s0)  (-> 0x8012d8e4)
      return;
    }
    if (c->mem_r8(0x800BF9DFu) != 0) {                      // 8012D8B4 lbu 0x16f(a0) ; 8012D8BC bnez -> 0x8012d8e4
      c->mem_w8(obj + 1, 1);                                // node[1]=1
      return;
    }
    uint8_t p = c->mem_r8(0x1F800207u);                     // 8012D8C8 lbu 0x207(1f80)
    // addiu v0,p,-0x1e ; sltiu v0,v0,2 ; bnez -> epilogue (when (p-0x1e) < 2, i.e. p in {0x1e,0x1f})
    if ((uint8_t)(p - 0x1e) < 2) return;                    // 8012D8D8 bnez -> 0x8012d8f4 epilogue
    c->mem_w8(obj + 1, 1);                                  // 8012D8E0/E8 node[1]=1
    return;
  }

  }  // switch(n3)
}
