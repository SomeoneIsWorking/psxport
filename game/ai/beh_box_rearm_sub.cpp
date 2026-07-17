// game/ai/beh_box_rearm_sub.cpp — PC-native per-object BEHAVIOR handler FUN_8013ADBC.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8013ADBC
// (scratch/decomp/field2/8013adbc.c). Outer state machine on node[4]:
//   STATE 0 : INIT — seed the size/box params node[0x80]=0x50, node[0x82]=0xA0, node[0x84]=200,
//             node[0x86]=400, node[0x40]=30; node[4]=1, node[0]=1, node[0x0B]=node[0x2B]=node[0x29]=
//             node[0x5E]=0.
//   STATE 1 : if FUN_8007778C(node)!=0 -> FUN_8013AC98(node); then node[0x29]=node[0x2B]=0.
//   STATE 2 : if node[5]==0 re-arm (node[4]=1, node[0]=1, node[0x29]=1, node[5]=node[0x5E]); then
//             FUN_8013AC98(node).
//   STATE 3 : FUN_8007A624(node).
//
// CONTROL FLOW + the direct node WRITES owned native; the sub-behavior CALLs (FUN_8013AC98, FUN_8007778C,
// FUN_8007A624) stay reachable via rec_dispatch (pure-PSX leaves). Store widths from the decompile
// (undefined2 = 16-bit sh, byte = sb). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call)
// is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "object/actor.h"     // Actor::boundsCull (FUN_8007778C — thin wrapper native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013ADBCu;

static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}

}  // namespace

void beh_box_rearm_sub(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    if (Actor(c, nd).boundsCull() != 0) guest_leaf(c, 0x8013ac98u, nd);   // FUN_8007778C native / FUN_8013AC98
    c->mem_w8(nd + 0x29, 0);
    c->mem_w8(nd + 0x2b, 0);
  } else if (st < 2) {
    if (st == 0) {
      c->mem_w16(nd + 0x80, 0x50);
      c->mem_w16(nd + 0x82, 0xa0);
      c->mem_w16(nd + 0x84, 200);
      c->mem_w16(nd + 0x86, 400);
      c->mem_w8(nd + 4, 1);
      c->mem_w8(nd + 0, 1);
      c->mem_w8(nd + 0x0b, 0);
      c->mem_w8(nd + 0x2b, 0);
      c->mem_w8(nd + 0x29, 0);
      c->mem_w8(nd + 0x5e, 0);
      c->mem_w16(nd + 0x40, 0x1e);
    }
  } else if (st == 2) {
    if (c->mem_r8(nd + 5) == 0) {
      c->mem_w8(nd + 4, 1);
      c->mem_w8(nd + 0, 1);
      c->mem_w8(nd + 0x29, 1);
      c->mem_w8(nd + 5, c->mem_r8(nd + 0x5e));
    }
    guest_leaf(c, 0x8013ac98u, nd);                                        // FUN_8013AC98
  } else if (st == 3) {
    eng(c).spawn.despawn(nd);                                        // FUN_8007A624
  }
}
