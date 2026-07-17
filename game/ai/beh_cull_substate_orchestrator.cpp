// game/ai/beh_cull_substate_orchestrator.cpp — PC-native per-object BEHAVIOR handler FUN_8013259C (OVERLAY).
//
// An OVERLAY-resident per-object behavior routine (lives in the field overlay, guest 0x8013xxxx),
// installed at node+0x1c and called every frame by the entity walk with the object node in a0. Same
// SHAPE as the resident siblings (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler): a state machine on
// the node's state byte node[4] (0 init / 1 active / 2 idle / 3 despawn). The orchestration lives in a
// handful of sub-functions it CALLS, which stay PSX leaves via rec_dispatch:
//   state 0  -> FUN_8013272C                          (per-type init; node[4]++ inside)
//   state 1  -> a cull GATE (FUN_8007778C, gated by node[3]/globals/timer), then a node[5] sub-state
//               machine calling FUN_80132954 / FUN_80132D58 / FUN_80132EDC / FUN_80133500, then a
//               common tail FUN_80133550 + FUN_80132A88 + per-node[1] FUN_800518FC, finally node[0x29]=0.
//   state 2  -> FUN_80133184
//   state 3  -> FUN_8007A624 (despawn)
//   other    -> epilogue (no-op)
//
// Ownership model (identical to the siblings): CONTROL FLOW + node/global memory writes owned native;
// every sub-behavior CALL stays a reachable PSX leaf via rec_dispatch (NO recursion into them). NO GTE,
// NO render packets here. RE'd 1:1 from the field RAM dump disas 0x8013259C..0x80132728 (jr ra at
// 0x80132724; the next function 0x8013272C has its own prologue). It WRITES guest node state the still-
// recomp content reads -> content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B vs rec_super_call).
// The idle/active field path is exercised by the gate; input/scene-driven sub-states are faithfully
// transcribed and verify when a scene drives them (same caveat as the sibling orchestrators).
//
// Globals referenced (computed from the lui/addiu pairs in the disasm):
//   0x800BF89C (lbu, state-1 cull gate; ==2)
//   0x800E7EAA (lbu, state-1 cull gate; ==1)
//   0x1F800160 (lh  scratchpad, state-1 cull gate; signed < 0x4651 => cull)

#include "core.h"
#include "game_ctx.h"
#include "render/render.h"       // Core::mRender (NodeXform)
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013259Cu;

}  // namespace

void beh_cull_substate_orchestrator(Core* c) {
  const uint32_t obj = c->r[4];                        // 0x801325A4  move s0, a0  (s0 = obj node ptr)
  uint8_t st = c->mem_r8(obj + 4);                     // 0x801325AC  lbu a0, 4(s0)  (a0 = node[4] = state)

  // ---- state dispatch [0x801325B0..0x801325F4] ----
  // NB: a0 still holds `st` through the state-1 cull gate below (no write to a0 until 0x80132658).
  if (st != 1) {                                       // 0x801325B4  beq a0,1 -> 0x80132600
    if (st >= 2) {                                     // 0x801325B8  slti v0,a0,2 / beqz -> 0x801325D4
      if (st == 2) {                                   // 0x801325D8  beq a0,2 -> 0x80132704
        c->r[4] = obj; rec_dispatch(c, 0x80133184u);   // 0x80132704  jal FUN_80133184 (a0=s0)
        return;                                        // 0x80132708  j 0x8013271C (epilogue)
      }
      if (st == 3) {                                   // 0x801325E0  beq a0,3 -> 0x80132714
        eng(c).spawn.despawn(obj);   // 0x80132714  jal FUN_8007a624 (a0=s0)  (despawn)
        return;                                        // 0x80132718  falls into epilogue
      }
      return;                                          // 0x801325E8  j 0x8013271C (other >=2 -> epilogue)
    }
    if (st != 0) return;                               // 0x801325C4  beqz a0 -> 0x801325F0; else j epilogue (0x801325CC)
    // ---- STATE 0 [0x801325F0] ----
    c->r[4] = obj; rec_dispatch(c, 0x8013272Cu);       // 0x801325F0  jal FUN_8013272C (a0=s0)  (per-type init)
    return;                                            // 0x801325F8  j 0x8013271C (epilogue)
  }

  // ---- STATE 1: cull GATE [0x80132600..0x8013265C] ----
  // The cull (FUN_8007778C) is invoked unless gated off. Decode the nested branch:
  //   reach the timer check (0x80132640) unless: n3 in {1,5} AND [0x800BF89C]!=2 AND [0x800E7EAA]==1
  //   at the timer check, cull iff (int16_t)scratch[0x160] < 0x4651.
  {
    uint8_t n3 = c->mem_r8(obj + 3);                   // 0x80132600  lbu v1, 3(s0)
    bool skip_cull = false;
    bool to_timer;                                     // reach the 0x80132640 timer check
    if (n3 == 1 || n3 == 5) {                          // 0x80132608 beq v1,a0(=1) ; 0x80132610 bne v1,5
      // 0x80132618: lbu v1, 0x800BF89C ; beq v1,2 -> 0x8013263C (to timer)
      if (c->mem_r8(0x800BF89Cu) == 2) {
        to_timer = true;
      } else {
        // 0x8013262C: lbu v0, 0x800E7EAA ; beq v0,a0(=1) -> 0x8013265C (skip cull)
        if (c->mem_r8(0x800E7EAAu) == 1) {
          skip_cull = true;                            // 0x80132634  beq -> 0x8013265C
          to_timer = false;
        } else {
          to_timer = true;                             // fall to 0x8013263C
        }
      }
    } else {
      to_timer = true;                                 // 0x80132610  bne v1,5 -> 0x80132640
    }
    if (!skip_cull && to_timer) {
      // 0x80132640: lh v0, 0x1F800160 (scratchpad, signed) ; slti v0,v0,0x4651 ; beqz -> 0x8013265C
      int16_t timer = c->mem_r16s(0x1F800160u);
      if (timer < 0x4651) {
        Actor(c, obj).boundsCull();                    // 0x80132654 jal FUN_8007778C (result IGNORED) — Actor::boundsCull (native)
      }
    }
  }

  // ---- STATE 1: node[5] sub-state machine [0x8013265C..0x801326D0] ----
  uint8_t sub = c->mem_r8(obj + 5);                    // 0x8013265C  lbu v1, 5(s0)
  switch (sub) {                                       // beq/slti tree, NOT a jump table
    case 0:                                            // 0x80132674  beqz v1 -> 0x8013269C
      c->r[4] = obj; rec_dispatch(c, 0x80132954u);     // 0x8013269C  jal FUN_80132954 (a0=s0)
      break;                                           // 0x801326A4  j 0x801326D4 (tail)
    case 1:                                            // 0x8013267C  j 0x801326D4 (no call)
      break;
    case 2:                                            // 0x80132664  beq v1,2 -> 0x801326AC
      c->r[4] = obj; rec_dispatch(c, 0x80132D58u);     // 0x801326AC  jal FUN_80132D58 (a0=s0)
      break;                                           // 0x801326B4  j 0x801326D4 (tail)
    case 3:                                            // 0x80132684  beq v1,3 -> 0x801326BC
      c->r[4] = obj; rec_dispatch(c, 0x80132EDCu);     // 0x801326BC  jal FUN_80132EDC (a0=s0)
      break;                                           // 0x801326C4  j 0x801326D4 (tail)
    case 4:                                            // 0x8013268C  beq v1,4 -> 0x801326CC
      c->r[4] = obj; rec_dispatch(c, 0x80133500u);     // 0x801326CC  jal FUN_80133500 (a0=s0)
      break;                                           // 0x801326D0  NO j; falls through to tail (same as code)
    default:                                           // sub>=5 -> 0x80132694 j 0x801326D4 (tail)
      break;
  }

  // ---- STATE 1: common tail [0x801326D4..0x80132700] ----
  c->r[4] = obj; rec_dispatch(c, 0x80133550u);         // 0x801326D4  jal FUN_80133550 (a0=s0)
  c->r[4] = obj; rec_dispatch(c, 0x80132A88u);         // 0x801326DC  jal FUN_80132A88 (a0=s0)
  uint8_t n1 = c->mem_r8(obj + 1);                     // 0x801326E4  lbu v0, 1(s0)
  c->mem_w8(obj + 0x29, 0);                            // 0x801326F0  sb zero, 0x29(s0)  (delay slot @ED-style; ALWAYS runs)
  if (n1 != 0) {                                       // 0x801326EC  beqz v0 -> 0x8013271C (epilogue)
    rend(c)->mNodeXform.buildWithOffset(obj);                  // FUN_800518FC (native)         [0x801326F4]
  }
  // 0x801326FC  j 0x8013271C (epilogue)
}
