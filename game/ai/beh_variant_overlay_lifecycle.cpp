// game/ai/beh_variant_overlay_lifecycle.cpp — PC-native per-object BEHAVIOR handler FUN_8007DC38.
//
// A RESIDENT MAIN.EXE per-object behavior (installed by FUN_8007E038, which sets node[0x1C]=this handler,
// node[0x47]=1, node[3]=variant, node[0x28]|=0x80, and seeds the record-table pointers node[0x48]/[0x4C]/
// [0x50] from the global list DAT_800ECF60, node[0x5C]=0xFFFF, node[0x5E]=record-index). A 4-phase
// lifecycle overlay actor (sibling of FUN_8007DDE0): it manages the global overlay-flag byte at
// 0x800BF822 (this instance owns bit 0x04), seeds its world position from a record table indexed by
// node[0x5E], and picks a variant anim/sprite id into node[0x18] from the variant byte node[3].
//
// Outer state machine on node[4] (uint8):
//   STATE 0 (spawn/init): pos = mem32(node+0x50) + (uint16)mem16( mem32(node+0x4C) + s16(node+0x5E)*4 );
//     write that to node[0x10] AND node[0x14]; FUN_8007C0D0(node,0); node[0x46]=1; node[4]++ (->1).
//     Then by variant byte node[3]: ==2 -> node[0x18]=5; ==0 -> node[0x18]=4 and OR bit 0x04 into
//     0x800BF822; ==3 -> node[0x18]=1; (==1 and >=4 do nothing).
//   STATE 1 (active): if node[3]==0 && (0x800BF822 & 0xFB)!=0 -> node[4]=2 (begin teardown). Always
//     FUN_8007C940(node); FUN_8007CC00(node); then if node[3]!=1 -> FUN_8005019C(node+0x54, node[0x18],
//     1, 2) (the per-frame anim/render step).
//   STATE 2: node[4]=3 (one-frame transition into despawn).
//   STATE 3 (despawn): if node[3]==0 -> clear bit 0x04 of 0x800BF822; FUN_8007A624(node) (despawn).
//
// CONTROL FLOW + every node-field / global-byte WRITE owned native at the exact offset/width; every
// sub-behavior CALL stays a pure-PSX leaf via rec_dispatch (set c->r[4..] then dispatch). node[3] is read
// as a byte (matching Ghidra: signed `char` only ever ==/!= tested in state 1, unsigned `byte` in the
// state-0 variant switch — equality/<3 are signedness-invariant here). node[0x5E] is a signed `short`,
// node[0x4C]/[0x50]/[0x10]/[0x14] are 32-bit, the table value is a `ushort`. Byte-exact A/B gate (full
// RAM+scratchpad vs rec_super_call) is the safety net.

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

constexpr uint32_t BEH_FN = 0x8007DC38u;
constexpr uint32_t OVL_FLAG = 0x800BF822u;   // DAT_800bf822 — global overlay-flag byte; this owns bit 0x04

}  // namespace

void beh_variant_overlay_lifecycle(Core* c) {
  const uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    // ---------- STATE 1 (active) ----------
    if (c->mem_r8(nd + 3) == 0 && (c->mem_r8(OVL_FLAG) & 0xfb) != 0) {
      c->mem_w8(nd + 4, 2);
    }
    guest_leaf(c, 0x8007c940u, nd);                          // FUN_8007C940
    guest_leaf(c, 0x8007cc00u, nd);                          // FUN_8007CC00
    if (c->mem_r8(nd + 3) != 1) {
      guest_leaf(c, 0x8005019cu, nd + 0x54, c->mem_r8(nd + 0x18), 1, 2);  // FUN_8005019C(node+0x54,node[0x18],1,2)
    }
  } else if (st < 2) {
    // ---------- STATE 0 (spawn/init) ----------  (st == 0)
    int32_t  base = (int32_t)c->mem_r32(nd + 0x4c);
    int16_t  idx  = c->mem_r16s(nd + 0x5e);
    uint16_t tv   = c->mem_r16(base + (uint32_t)(int32_t)(idx * 4));
    int32_t  pos  = (int32_t)c->mem_r32(nd + 0x50) + (int32_t)(uint32_t)tv;
    c->mem_w32(nd + 0x10, (uint32_t)pos);
    c->mem_w32(nd + 0x14, (uint32_t)pos);
    guest_leaf(c, 0x8007c0d0u, nd, 0);                       // FUN_8007C0D0(node,0)
    uint8_t n3 = c->mem_r8(nd + 3);
    c->mem_w8(nd + 0x46, 1);
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));     // node[4] += 1  (-> 1)
    if (n3 == 2) {
      c->mem_w8(nd + 0x18, 5);
    } else if (n3 < 3) {
      if (n3 == 0) {
        c->mem_w8(nd + 0x18, 4);
        c->mem_w8(OVL_FLAG, (uint8_t)(c->mem_r8(OVL_FLAG) | 4));
      }
    } else if (n3 == 3) {
      c->mem_w8(nd + 0x18, 1);
    }
  } else if (st == 2) {
    // ---------- STATE 2 (transition) ----------
    c->mem_w8(nd + 4, 3);
  } else if (st == 3) {
    // ---------- STATE 3 (despawn) ----------
    if (c->mem_r8(nd + 3) == 0) {
      c->mem_w8(OVL_FLAG, (uint8_t)(c->mem_r8(OVL_FLAG) & 0xfb));
    }
    eng(c).spawn.despawn(nd);                              // FUN_8007A624 (despawn)
  }
}
