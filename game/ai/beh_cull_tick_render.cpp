// engine/beh_cull_tick_render.cpp — PC-native per-object BEHAVIOR handler FUN_8012D404 (OVERLAY).
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
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn
#include "graphics_bind.h"   // ov_obj_record_init
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012D404u;

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
        world_despawn(c, obj);  // 8012D4D4 jal 0x8007a624 (a0=s0)
      }
      return;                                      // 8012D4DC epilogue (state 2 / other)
    }
    // 8012D42C beqz v1 -> state 0 (0x8012d458); delay 8012D430 move a0,s0
    if (st != 0) return;                           // 8012D434 j 0x8012d4dc (v1<2 && !=0: dead path, epilogue)

    // ---- STATE 0 [0x8012D458]: cull-record init + node+0x54 seed ----
    uint8_t n3a = c->mem_r8(obj + 3);              // 8012D45C lbu v0, 3(s0)
    // 8012D458 lui v1,0x8015 ; 8012D460 addiu v1,-0x5da0 -> 0x8014A260
    // 8012D464 sll v0,1 ; 8012D468 addu ; 8012D46C lh a2,(v0)  (SIGNED halfword)
    int16_t tv = (int16_t)c->mem_r16(0x8014A260u + (uint32_t)n3a * 2);
    c->r[4] = obj; c->r[5] = 0xc; c->r[6] = (uint32_t)(int32_t)tv;
    ov_obj_record_init(c);                  // 8012D470 jal 0x80051b70 ; 8012D474 (delay) a1=0xc
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
  c->r[4] = obj; rec_dispatch(c, 0x8007778Cu);     // 8012D4AC jal 0x8007778c (a0=s0)  cull
  if (c->r[2] == 0) return;                         // 8012D4B4 beqz v0 -> 0x8012d4dc (culled -> epilogue)
  c->r[4] = obj; rec_dispatch(c, 0x8012D27Cu);     // 8012D4BC jal 0x8012d27c (a0=s0)  per-type tick
  c->r[4] = obj; ov_obj_render_update(c);     // 8012D4C4 jal 0x800517f8 (a0=s0)  render-state update
  // 8012D4CC j 0x8012d4dc (epilogue)
}

void ov_beh_cull_tick_render(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("cull_tick_renderverify") ? 1 : 0;
  if (!s_v) { beh_cull_tick_render(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_cull_tick_render(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[cull_tick_renderverify] MISMATCH obj=%08x st=%u sub=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[cull_tick_renderverify] %ld matches\n", ng);
}

}  // namespace

void beh_cull_tick_render_register(void) {
}

// Exported entry — the verify wrapper ov_beh_cull_tick_render is in the anonymous namespace above (internal
// linkage); the engine's per-object dispatch calls THIS to run the owned behavior.
void ov_beh_cull_tick_render_run(Core* c) { ov_beh_cull_tick_render(c); }
