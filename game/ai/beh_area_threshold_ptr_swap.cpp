// game/ai/beh_area_threshold_ptr_swap.cpp — PC-native per-object BEHAVIOR handler FUN_8013C3F4.
//
// 2nd-hottest still-PSX OVERLAY handler (~x4632/field-frame on seaside; ~80 instr), an area-overlay
// routine (prologue 0x8013C3F4; `jr ra` at 0x8013C530). Lives only at runtime in the area overlay
// (NOT in MAIN.EXE) — disassembled from scratch/ram/field_seaside.bin. State machine on node[4]:
//
//   STATE >=4 : nothing (exit).            STATE 2/3 : FUN_8007A624(node), exit.
//   STATE 0   : area byte 0x800BF9E0 >= 28 -> node[4]=3 exit; else node[4]=1 (16-bit) and FALL into
//               state 1.
//   STATE 1   : if byte 0x800E7FEB == 1 exit. node[0x34] = node[0x38]; if node[0x38] != 0 skip to the
//               tail. Else node[0x34] = 0x8014AC18 (an overlay data ptr); if node[3]==128 -> node[0x34]=0,
//               node[4]=3, exit; else pick a threshold on 0x800BF9E0 by node[3] (< 6 if node[3]<2, else
//               < 16) — when NOT below it, node[0x34] = 0x8014AF20 and node[3] = 128. Tail: if byte
//               0x800E7EAA >= 12 -> node[4]=3 exit; else FUN_8002B278(node) nonzero -> exit, else
//               FUN_80031780(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the node memory WRITES owned native; the
// 3 sub-behavior CALLs (FUN_8002B278, FUN_80031780, FUN_8007A624) stay reachable via rec_dispatch
// (pure-PSX leaf). NO GTE, NO render packets. Transcribed 1:1 as a register machine (locals = guest
// regs, goto labels = guest addresses) so delay-slot effects stay exact (e.g. the c454 `sh a0,4`
// 16-bit state store; the c494 `sltiu v0,v0,2` computed in a branch delay then read at L4a8). a0 is
// written into c->r only for the leaf calls (= node, where the guest writes it). v0 (handler return)
// is NOT reproduced; the gate compares only RAM+scratchpad vs rec_super_call.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::coneCull2b278 (FUN_8002B278)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "collision.h"  // Collision::listScan (FUN_80031780)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013C3F4u;

}  // namespace

void beh_area_threshold_ptr_swap(Core* c) {
  uint32_t obj = c->r[4];                        // s0 = a0 (node)
  uint32_t v0;

  uint8_t st = c->mem_r8(obj + 4);               // node[4] = state
  if (st == 1) goto L458;
  if (st < 2) { if (st == 0) goto L440; goto L528; }  // st<2 -> only st==0 reachable
  if (st < 4) goto L520;                              // st in {2,3}
  goto L528;                                          // st >= 4 default

 L520:                                           // STATE 2/3 — FUN_8007A624(node)
  eng(c).spawn.despawn(obj);
  goto L528;

 L440:                                           // STATE 0
  if (!(c->mem_r8(0x800BF9E0u) < 28)) goto L518; // >= 28 -> node[4]=3
  c->mem_w16(obj + 4, 1);                        // sh a0(=1),4 — 16-bit: node[4]=1, node[5]=0
  // fall through to STATE 1

 L458:                                           // STATE 1
  if (c->mem_r8(0x800E7FEBu) == 1) goto L528;
  v0 = c->mem_r32(obj + 0x38);
  c->mem_w32(obj + 0x34, v0);                    // node[0x34] = node[0x38] (c478 delay)
  if (v0 != 0) goto L4e0;
  // node[0x38] == 0
  c->mem_w32(obj + 0x34, 0x8014AC18u);
  {
    uint8_t n3 = c->mem_r8(obj + 3);
    if (n3 == 128) {                             // L498
      c->mem_w32(obj + 0x34, 0);
      c->mem_w8(obj + 4, 3);
      goto L528;
    }
    uint32_t cond = (n3 < 2)                      // L4a8
                      ? (c->mem_r8(0x800BF9E0u) < 6  ? 1u : 0u)   // L4b0
                      : (c->mem_r8(0x800BF9E0u) < 16 ? 1u : 0u);  // L4bc
    if (cond == 0) {                              // L4d0
      c->mem_w32(obj + 0x34, 0x8014AF20u);
      c->mem_w8(obj + 3, 128);
    }
  }
 L4e0:
  if (!(c->mem_r8(0x800E7EAAu) < 12)) goto L518; // >= 12 -> node[4]=3
  c->r[4] = obj; eng(c).cull.coneCull2b278();   // FUN_8002B278 (native)
  if (c->r[2] != 0) goto L528;
  eng(c).collision.listScan(obj);
  goto L528;
 L518:
  c->mem_w8(obj + 4, 3);
 L528:
  return;
}
