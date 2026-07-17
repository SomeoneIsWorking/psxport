// game/ai/beh_id_routed_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80121978.
//
// Overlay handler (hottest still-PSX overlay handler ~x1592/field-frame on seaside; ~115 instr),
// prologue 0x80121978; `jr ra` at 0x80121B3C. Disassembled from scratch/ram/field_seaside.bin.
// Outer state machine on node[4] (a0):
//   STATE 0 : INIT — FUN_800519E0(node,18,*0x800ECFCC,0x8014C02C); on 0, FUN_80077C40(node,0x8014E4EC,0)
//             with node[0x3C]=*0x800ECFD0, then seed node[0x80..0x86]=140/280/128/256, node[0x44]=384,
//             node[11]=node[0x2B]=node[0x47]=0, and node[4] += 1.
//   STATE 1 : route node[3] to a per-id sub-behavior leaf (0->FUN_801225BC, 1->FUN_80122D58,
//             95->FUN_801220FC, 96->FUN_80121B44, 97->FUN_80121CF8, 98->FUN_80122CA4, 99->FUN_8018BF08;
//             all other ids none), then ALWAYS FUN_80122BF4(node) + node[0x2B]=0.
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). No leaf here takes a stack
// argument, so no frame mirroring is needed. The INIT data words live in resident RAM (read live, not
// embedded). Transcribed 1:1 as a register machine; the byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. NO GTE/render.

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

constexpr uint32_t BEH_FN = 0x80121978u;

}  // namespace

void beh_id_routed_dispatch(Core* c) {
  uint32_t s0 = c->r[4];                            // s0 = a0 (node)
  uint32_t st = c->mem_r8(s0 + 4);                  // node[4] = outer state

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) goto S3;
  goto Lret;                                        // st >= 4

 // ---------------- STATE 0 (INIT) ----------------
 S0:
  guest_leaf(c, 0x800519E0u, s0, 18, c->mem_r32(0x800ECFCCu), 0x8014C02Cu);  // FUN_800519E0
  if (c->r[2] != 0) goto Lret;
  c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECFD0u));   // node[0x3C] = *0x800ECFD0 (delay-slot store)
  guest_leaf(c, 0x80077C40u, s0, 0x8014E4ECu, 0);   // FUN_80077C40(node, 0x8014E4EC, 0)
  c->mem_w16(s0 + 0x80, 140);
  c->mem_w16(s0 + 0x82, 280);
  c->mem_w16(s0 + 0x84, 128);
  c->mem_w16(s0 + 0x86, 256);
  c->mem_w8 (s0 + 11, 0);
  c->mem_w8 (s0 + 0x2B, 0);
  c->mem_w16(s0 + 0x44, 384);
  c->mem_w8 (s0 + 0x47, 0);
  c->mem_w8 (s0 + 4, (uint8_t)(c->mem_r8(s0 + 4) + 1));  // node[4] += 1
  goto Lret;

 // ---------------- STATE 1 ----------------
 S1:
  switch (c->mem_r8(s0 + 3)) {                       // node[3]
    case 0:  guest_leaf(c, 0x801225BCu, s0); break;
    case 1:  guest_leaf(c, 0x80122D58u, s0); break;
    case 95: guest_leaf(c, 0x801220FCu, s0); break;
    case 96: guest_leaf(c, 0x80121B44u, s0); break;
    case 97: guest_leaf(c, 0x80121CF8u, s0); break;
    case 98: guest_leaf(c, 0x80122CA4u, s0); break;
    case 99: guest_leaf(c, 0x8018BF08u, s0); break;
    default: break;                                  // 2..94, 100..255: no sub-behavior
  }
  guest_leaf(c, 0x80122BF4u, s0);                     // common tail FUN_80122BF4(node)
  c->mem_w8(s0 + 0x2B, 0);                            // node[0x2B] = 0 (delay-slot store)
  goto Lret;

 // ---------------- STATE 3 ----------------
 S3:
  eng(c).spawn.despawn(s0);                          // FUN_8007A624(node)
 Lret:
  return;
}
