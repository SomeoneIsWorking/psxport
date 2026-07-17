// game/ai/beh_cull_tick_render.cpp — PC-native per-object BEHAVIOR handler FUN_8012D404 (OVERLAY).
//
// An OVERLAY-resident per-object behavior routine (lives in the field overlay, not MAIN.EXE), installed
// at node+0x1c and called every frame by the entity walk with the object node in a0. Same SHAPE as the
// resident siblings (the FUN_739ac handler / the FUN_73cd8 handler) and the overlay siblings (the FUN_8012eb54 handler /
// the FUN_80124e74 handler): a state machine on the node's state byte node[4] (0 init / 1 active / 2 idle /
// 3 despawn). It is SMALL and has NO jump table — a flat per-state body:
//   state 0  -> cull-record init via FUN_80051b70 (a1=0xc, a2 = signed table[node3]); on success node[4]++
//               and seed node+0x54 from a second per-node3 table.
//   state 1  -> cull FUN_8007778c; if NOT culled, FUN_8012d27c (per-type tick) then FUN_800517f8 (render).
//   state 2  -> return (idle).
//   state 3  -> FUN_8007a624 (despawn).
//
// Two per-node3 DATA tables (NOT jump tables — plain halfword arrays indexed by node[3]):
//   0x8014A260 (lh,  signed)   -> a2 for the FUN_80051b70 init call   [lui 0x8015 / addiu -0x5da0]
//   0x8014A268 (lhu, unsigned) -> seed for node+0x54                  [lui 0x8015 / addiu -0x5d98]
//
// Ownership model (identical to the FUN_739ac handler / the FUN_73cd8 handler / the FUN_8012eb54 handler / the FUN_80124e74 handler):
// CONTROL FLOW + node/global memory writes owned native; every sub-behavior CALL stays reachable by
// address via rec_dispatch (a0..a2 set first; NO recursion into them). NO GTE, NO render packets here.
// RE'd 1:1 from disas 0x8012D404..0x8012D4E8 (`jr ra`; 0x8012D4EC starts the next fn with its own
// prologue). It WRITES guest node state the still-recomp content reads -> content-INTERFACE: gated
// byte-exact (full RAM+scratchpad A/B vs rec_super_call). The idle/active field path is exercised by the
// gate; the scene-driven init is faithfully transcribed and verifies when a scene drives it.

#include "core.h"
#include "game_ctx.h"
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_record_init
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012D404u;

}  // namespace

void beh_cull_tick_render(Core* c) {
  const uint32_t obj = c->r[4];                    // 8012D40C move s0, a0  (s0 = obj node ptr)
  uint8_t st = c->mem_r8(obj + 4);                 // 8012D414 lbu v1, 4(s0)  (state byte)

  // ---- state dispatch [0x8012D418..0x8012D454] ----
  // 8012D41C beq v1,1 -> state 1 (0x8012d4ac)
  // 8012D420 slti v0,v1,2 ; 8012D424 beqz v0 -> v1>=2 (0x8012d43c)
  if (st != 1) {
    if (st >= 2) {                                 // ---- 0x8012D43C: v1 >= 2 ----
      // 8012D440 beq v1,2 -> 0x8012d4dc (epilogue, idle no-op)
      // 8012D448 beq v1,3 -> 0x8012d4d4 (despawn) ; else 8012D450 j 0x8012d4dc
      if (st == 3) {
        eng(c).spawn.despawn(obj);  // 8012D4D4 jal 0x8007a624 (a0=s0)
      }
      return;                                      // 8012D4DC epilogue (state 2 / other)
    }
    // 8012D42C beqz v1 -> state 0 (0x8012d458); delay 8012D430 move a0,s0
    if (st != 0) return;                           // 8012D434 j 0x8012d4dc (v1<2 && !=0: dead path, epilogue)

    // ---- STATE 0 [0x8012D458]: cull-record init + node+0x54 seed ----
    uint8_t n3a = c->mem_r8(obj + 3);              // 8012D45C lbu v0, 3(s0)
    // 8012D458 lui v1,0x8015 ; 8012D460 addiu v1,-0x5da0 -> 0x8014A260
    // 8012D464 sll v0,1 ; 8012D468 addu ; 8012D46C lh a2,(v0)  (SIGNED halfword)
    int16_t tv = c->mem_r16s(0x8014A260u + (uint32_t)n3a * 2);
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = (uint32_t)(int32_t)tv;
    eng(c).graphicsBind.recordInit();                  // 8012D470 jal 0x80051b70 ; 8012D474 (delay) a1=0xc
    if (c->r[2] != 0) return;                       // 8012D478 bnez v0 -> 0x8012d4dc (init busy/failed -> epilogue)

    c->mem_w8(obj + 4, (uint8_t)(c->mem_r8(obj + 4) + 1));  // 8012D480..8C lbu v0,4(s0); +1; sb v0,4(s0) -> state 1
    uint8_t n3b = c->mem_r8(obj + 3);              // 8012D490 lbu v0, 3(s0)
    // 8012D494 addiu v1,-0x5d98 (base 0x8015) -> 0x8014A268
    // 8012D498 sll v0,1 ; 8012D49C addu ; 8012D4A0 lhu v0,(v0)  (ZERO-extend halfword)
    uint16_t sv = (uint16_t)c->mem_r16(0x8014A268u + (uint32_t)n3b * 2);
    c->mem_w16(obj + 0x54, sv);                    // 8012D4A8 sh v0, 0x54(s0)  (delay slot of j epilogue)
    return;                                        // 8012D4A4 j 0x8012d4dc (epilogue)
  }

  // ---- STATE 1 [0x8012D4AC]: cull, then tick + render ----
  if (Actor(c, obj).boundsCull() == 0) return;      // 8012D4AC jal 0x8007778c — Actor::boundsCull (native); cull → epilogue
  c->r[4] = obj; rec_dispatch(c, 0x8012D27Cu);     // 8012D4BC jal 0x8012d27c (a0=s0)  per-type tick
  c->r[4] = obj; eng(c).graphicsBind.renderUpdate();     // 8012D4C4 jal 0x800517f8 (a0=s0)  render-state update
  // 8012D4CC j 0x8012d4dc (epilogue)
}
