// game/ai/beh_linked_advance_branch.cpp — PC-native per-object BEHAVIOR handler FUN_80128760.
//
// Overlay handler (~x1556/field-frame on seaside; ~95 instr), prologue 0x80128760; `jr ra` at
// 0x801288D0. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : FUN_80128308(node).
//   STATE 1 : node[3]==0 -> branch A, node[3]==1 -> branch B, else nothing. Both branches gate an
//             "advance node[5]" reset (node[11]=0, node[0x10]=0, node[5]+=1) on the linked object
//             node[0x10]: when node[5]==0 and (node[0x10]==0 OR node[0x10][0x5E]==2) the reset runs.
//             Branch A then -> FUN_801281B8(node) (Ltail); when node[5]!=0 it only runs Ltail if
//             node[5]==1. Branch B then drops to a scratchpad gate: if scratchpad[0x207] < 6, run
//             FUN_801281B8(node)+FUN_801285EC(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf, a0=node). Transcribed 1:1 as
// a register machine (the two near-identical branches share the L883c node[5]==1 test). The byte-exact
// A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80128760u;

}  // namespace

void beh_linked_advance_branch(Core* c) {
  uint32_t s0 = c->r[4];                            // s0 = a0 (node)
  uint32_t v1 = c->mem_r8(s0 + 4);                  // node[4] = outer state
  uint8_t n5;
  uint32_t rec;

  if (v1 == 1) goto S1;
  if ((int32_t)v1 < 2) { if (v1 == 0) { guest_leaf(c, 0x80128308u, s0); } goto Lret; }  // STATE 0
  if (v1 == 2) goto Lret;                           // STATE 2 nothing
  if (v1 == 3) { eng(c).spawn.despawn(s0); goto Lret; }  // STATE 3
  goto Lret;                                        // v1 >= 4

 // ---------------- STATE 1 ----------------
 S1: {
    uint8_t n3 = c->mem_r8(s0 + 3);
    if (n3 == 0) goto BA;                            // 0x801287e4
    if (n3 == 1) goto BB;                            // 0x8012882c (n3 == node[4] == 1)
    goto Lret;
  }

 BA:                                                // node[3]==0
  n5 = c->mem_r8(s0 + 5);
  if (n5 != 0) goto L883c;
  rec = c->mem_r32(s0 + 0x10);                       // node[0x10]
  if (rec != 0) { if (c->mem_r8(rec + 0x5E) != 2) goto Ltail88b0; }
  // 0x80128814: advance reset -> then Ltail
  c->mem_w8 (s0 + 11, 0);
  c->mem_w32(s0 + 0x10, 0);
  c->mem_w8 (s0 + 5, (uint8_t)(n5 + 1));
  goto Ltail88b0;

 L883c:                                             // node[5]!=0 (shared by A and B)
  if (c->mem_r8(s0 + 5) == 1) goto Ltail88b0;
  goto Lret;

 BB:                                                // node[3]==1
  n5 = c->mem_r8(s0 + 5);
  if (n5 != 0) goto L883c;
  // 0x8012884c
  rec = c->mem_r32(s0 + 0x10);
  if (rec != 0 && c->mem_r8(rec + 0x5E) != 2) goto L88884;
  // 0x8012886c: advance reset -> fall to L88884
  c->mem_w8 (s0 + 11, 0);
  c->mem_w32(s0 + 0x10, 0);
  c->mem_w8 (s0 + 5, (uint8_t)(n5 + 1));
 L88884:
  if (c->mem_r8(0x1F800207u) < 6) {                  // scratchpad byte
    guest_leaf(c, 0x801281B8u, s0);
    guest_leaf(c, 0x801285ECu, s0);
  }
  goto Lret;

 Ltail88b0:
  guest_leaf(c, 0x801281B8u, s0);                    // FUN_801281B8(node)
 Lret:
  return;
}
