// game/ai/beh_area_event_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80071A3C.
//
// A RESIDENT MAIN.EXE per-object behavior routine (prologue 0x80071A3C; `jr ra` at 0x80071B3C).
// Same SHAPE as the sibling owned behaviors (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8004c238 handler): a state
// machine on the node's state byte node[4]. RE'd 1:1 from disas 0x80071A3C:
//
//   STATE 0 : FUN_800716B4(node).  Then a global-flag-gated event dispatch:
//             if  (byte@0x800BFAE1 != 0 && area byte@0x800BF870 == 4) FUN_8011B79C(node);   // overlay
//             elif(byte@0x800BFAE6 != 0 && area byte@0x800BF870 == 7) FUN_801178E4(node);   // overlay
//   STATE 1 : FUN_80071768(node).  if (node[1] != 0) FUN_800518FC(node).
//   STATE 2 : nothing.
//   STATE 3 : FUN_8007A624(node).
//   default : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the global-flag reads owned native;
// every sub-behavior CALL stays reachable by address via rec_dispatch (leaf, no recursion). This
// handler itself performs NO node/global memory WRITES — the callees do — so it is trivially
// content-interface-correct, but it is still gated byte-exact (full RAM+scratchpad A/B vs
// rec_super_call) like every owned behavior. NO GTE, NO render packets here.
//
// v0 is NOT reproduced: the per-object dispatcher (call_handler) ignores the handler return, and the
// behavior gate (below) compares only RAM+scratchpad — matching the sibling objbeh gates.

#include "core.h"
#include "game_ctx.h"
#include "render/render.h"       // Core::mRender (NodeXform)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80071A3Cu;

}  // namespace

void beh_area_event_dispatch(Core* c) {
  uint32_t obj = c->r[4];
  uint8_t state = c->mem_r8(obj + 4);
  switch (state) {
    case 0:
      c->r[4] = obj; rec_dispatch(c, 0x800716B4u);
      if (c->mem_r8(0x800BFAE1u) != 0 && c->mem_r8(0x800BF870u) == 4) {
        c->r[4] = obj; rec_dispatch(c, 0x8011B79Cu);
      } else if (c->mem_r8(0x800BFAE6u) != 0 && c->mem_r8(0x800BF870u) == 7) {
        c->r[4] = obj; rec_dispatch(c, 0x801178E4u);
      }
      break;
    case 1:
      c->r[4] = obj; rec_dispatch(c, 0x80071768u);
      if (c->mem_r8(obj + 1) != 0) rend(c)->mNodeXform.buildWithOffset(obj);   // FUN_800518FC (native)
      break;
    case 3:
      eng(c).spawn.despawn(obj);
      break;
    default:  // state 2 and any other: no-op
      break;
  }
}
