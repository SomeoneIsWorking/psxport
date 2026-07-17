// game/ai/beh_scatter_record_dither.cpp — PC-native per-object BEHAVIOR handler FUN_8013C538.
//
// The HOTTEST still-PSX OVERLAY handler (~x6091/field-frame on seaside), an area-overlay routine
// (prologue 0x8013C538; `jr ra` at 0x8013C7E8). Lives only at runtime in the area overlay (NOT in
// MAIN.EXE) — disassembled from scratch/ram/field_seaside.bin. Same ownership model as the resident
// siblings (the FUN_8006f2d0 handler / the FUN_8004ce14 handler / …): a state machine on the node's state byte node[4].
//
//   STATE >=4 : nothing (exit).            STATE 2/3 : FUN_8007A624(node), exit.
//   STATE 0   : read area byte 0x800BF9E0; <28 ? choose a small scatter count (node[0x4e]=7,n=7 when
//               <6, else node[0x4e]=1,n=1) and seed n stride-8 records at node[0x50] with random
//               offsets via FUN_80032A44(a0,a1); set node[4]=1, then FALL INTO state-1 logic.
//               (If 0x800BF9E0 >= 28 -> node[4]=3, exit.)
//   STATE 1   : a0=0x800E7E80 area block. If byte[+363]==1 exit. If byte[+42] >= 12 -> node[4]=3 exit;
//               else node[0x34]=node[0x38]; if node[0x38]==0 -> node[4]=3 exit. Else read 0x800BF9E0
//               again: <6 picks the &3 jitter loop (counts -1/-14/-2), >=6 the &7 loop (-3/-14/-4),
//               each running node[0x4e] iterations of 3x FUN_8009A450() to dither the node[0x50] recs.
//               Then FUN_8002B278(node): nonzero -> exit; else FUN_80031780(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the node memory WRITES owned native;
// every sub-behavior CALL (FUN_80032A44 scatter-rng, FUN_8009A450 rng, FUN_8002B278, FUN_80031780,
// FUN_8007A624) stays reachable by address via rec_dispatch (pure-PSX leaf, no recursion). NO GTE,
// NO render packets. Transcribed 1:1 as a register machine (locals = guest regs, goto labels = guest
// addresses) so delay-slot effects stay exact; the byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. a0/a1 are written into c->r ONLY where the guest writes them, so
// c->r evolves identically to the recomp across the leaf rec_dispatch calls (the no-arg FUN_8009A450
// calls inherit whatever the prior leaf left, exactly as the recomp does). v0 (handler return) is NOT
// reproduced (the per-object dispatcher ignores it; the gate compares only RAM+scratchpad).

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::coneCull2b278 (FUN_8002B278)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "rng.h"       // class Rng (via rngOf(c).next())
#include "collision.h"  // Collision::listScan (FUN_80031780)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013C538u;

}  // namespace

void beh_scatter_record_dither(Core* c) {
  uint32_t obj = c->r[4];                        // s3 = a0 (node)
  uint32_t v0, v1;
  int s0 = 0, s1 = 0, s2 = 0;                    // guest s0/s2 = running record ptrs, s1 = counter
  // c558 sets a0=1 right after the prologue; it survives only into the STATE 0 < 28 / >= 6 branch
  // (node[0x4e] = a0 = 1) — handled inline there. We don't mirror it into c->r since no leaf reads
  // a0 before it is overwritten (L63c writes a0 = 0x800E7E80).

  uint8_t st = c->mem_r8(obj + 4);               // node[4] = state
  if (st == 1) goto L63c;
  if (st < 2) { if (st == 0) goto L590; goto L7d4; }  // st<2 -> only st==0 reachable
  if (st < 4) goto L7cc;                              // st in {2,3}
  goto L7d4;                                          // st >= 4 default

 L7cc:                                           // STATE 2/3 — FUN_8007A624(node)
  eng(c).spawn.despawn(obj);
  goto L7d4;

 L590:                                           // STATE 0 — seed the scatter record list
  v1 = c->mem_r8(0x800BF9E0u);
  if (!(v1 < 28)) goto L678;                     // >= 28 -> node[4]=3
  if (v1 < 6) { c->mem_w16(obj + 0x4e, 7); s1 = 7; }
  else        { c->mem_w16(obj + 0x4e, 1); s1 = 1; }   // a0==1 here
  if (s1 <= 0) goto L634;
  s2 = (int)(obj + 0x50);
  s0 = (int)(obj + 0x56);
 L5cc:
  // FUN_80032A44 = Rng::inRange (native). Each call scales the shared PSX rand() into a range.
  v0 = rngOf(c).inRange(0, 128);
  s1 -= 1;
  v1 = (uint16_t)(c->mem_r16(obj + 0x2c) + v0);
  c->mem_w16((uint32_t)s2 + 0, v1);
  v0 = rngOf(c).inRange(-128, 0);
  v1 = (uint16_t)(c->mem_r16(obj + 0x2e) + v0);
  s2 += 8;
  c->mem_w16((uint32_t)s0 - 4, v1);
  v0 = rngOf(c).inRange(0, 32);
  v1 = (uint16_t)(c->mem_r16(obj + 0x30) + v0);
  c->mem_w16((uint32_t)s0 - 2, v1);
  v0 = rngOf(c).inRange(224, 288);
  c->mem_w16((uint32_t)s0 + 0, (uint16_t)v0);
  s0 += 8;                                       // c630 delay slot (always executes)
  if (s1 > 0) goto L5cc;
 L634:
  c->mem_w8(obj + 4, 1);
  // fall through to STATE 1

 L63c:                                           // STATE 1
  c->r[4] = 0x800E7E80u;                         // a0 = area block base
  v1 = c->mem_r8(0x800E7FEBu);                   // lbu 363(a0)
  if (v1 == 1) goto L7d4;
  v0 = c->mem_r8(0x800E7EAAu);                   // lbu 42(a0)
  if (!(v0 < 12)) goto L678;                     // >= 12 -> node[4]=3
  v0 = c->mem_r32(obj + 0x38);
  c->mem_w32(obj + 0x34, v0);                    // node[0x34] = node[0x38]
  if (v0 != 0) goto L684;
  // node[0x38] == 0 -> fall into L678
 L678:
  c->mem_w8(obj + 4, 3);
  goto L7d4;

 L684:
  v0 = c->mem_r8(0x800BF9E0u);
  if (!(v0 < 6)) goto L728;                      // >= 6 -> the &7 jitter loop
  s2 = (int)(obj + 0x50);
  if (c->mem_r16s(obj + 0x4e) <= 0) goto L7ac;
  s1 = 0;
  s0 = (int)(obj + 0x54);
 L6b0:
  v0 = (uint32_t)rngOf(c).next();
  s1 += 1;
  v1 = (uint16_t)(c->mem_r16((uint32_t)s2 + 0) - 1 + (uint32_t)(((int32_t)v0 >> 7) & 3));
  c->mem_w16((uint32_t)s2 + 0, v1);
  v0 = (uint32_t)rngOf(c).next();
  v1 = (uint16_t)(c->mem_r16((uint32_t)s0 - 2) - 14 - (uint32_t)(((int32_t)v0 >> 8) & 0xf));
  c->mem_w16((uint32_t)s0 - 2, v1);
  v0 = (uint32_t)rngOf(c).next();
  s2 += 8;
  v1 = (uint16_t)(c->mem_r16((uint32_t)s0 + 0) - 2 + (uint32_t)(((int32_t)v0 >> 7) & 3));
  c->mem_w16((uint32_t)s0 + 0, v1);
  s0 += 8;                                       // c71c delay slot
  if (s1 < c->mem_r16s(obj + 0x4e)) goto L6b0;
  goto L7ac;
 L728:
  if (c->mem_r16s(obj + 0x4e) <= 0) goto L7ac;
  s1 = 0;
  s0 = (int)(obj + 0x54);
 L73c:
  v0 = (uint32_t)rngOf(c).next();
  s1 += 1;
  v1 = (uint16_t)(c->mem_r16((uint32_t)s2 + 0) - 3 + (uint32_t)(((int32_t)v0 >> 7) & 7));
  c->mem_w16((uint32_t)s2 + 0, v1);
  v0 = (uint32_t)rngOf(c).next();
  v1 = (uint16_t)(c->mem_r16((uint32_t)s0 - 2) - 14 - (uint32_t)(((int32_t)v0 >> 8) & 0xf));
  c->mem_w16((uint32_t)s0 - 2, v1);
  v0 = (uint32_t)rngOf(c).next();
  s2 += 8;
  v1 = (uint16_t)(c->mem_r16((uint32_t)s0 + 0) - 4 + (uint32_t)(((int32_t)v0 >> 7) & 7));
  c->mem_w16((uint32_t)s0 + 0, v1);
  s0 += 8;                                       // c7a8 delay slot
  if (s1 < c->mem_r16s(obj + 0x4e)) goto L73c;
 L7ac:
  c->r[4] = obj; eng(c).cull.coneCull2b278();   // FUN_8002B278 (native)
  if (c->r[2] != 0) goto L7d4;
  eng(c).collision.listScan(obj);
 L7d4:
  return;
}
