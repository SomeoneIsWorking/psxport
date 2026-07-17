// game/ai/beh_substate_edge_orchestrator.cpp — PC-native per-object BEHAVIOR handler FUN_8012EB54 (OVERLAY).
//
// An OVERLAY per-object behavior routine (guest 0x8012xxxx) with the same state-byte shape as the
// resident siblings (the FUN_739ac handler / the FUN_73cd8 handler / the FUN_741dc handler): a state machine on the node's state
// byte node[4] (0 init / 1 active / 2 idle / 3 despawn). It is much larger than the resident ones — most
// of the work lives in ~14 sub-functions it CALLS, which stay PSX leaves via rec_dispatch:
//   state 0  -> FUN_8012ed84                          (per-type init: seeds node+0x60 table, node[4]++ etc.)
//   state 1  -> FUN_80130ac4 (cull-ish gate; if !=0 node[1]=state, FUN_80077ebc), then a node[5]
//               sub-state machine via jump table 0x80109dec (node[5] in [0,5]) calling
//               FUN_8012f494 / FUN_8012f5b4 / FUN_8012fd88 / FUN_80130524 / FUN_801313c4 / FUN_8018c820,
//               then a common tail: FUN_80131134, FUN_801316cc, a global-driven node[0x78]/node[5]/node[6]
//               edge machine keyed on DAT_800bf9d2, a node[0x48]->node[0x4e] sign clamp, then per-node[3]
//               FUN_80146348 and per-node[1] FUN_8012e8a8, finally node[0x2b]=0.
//   state 2  -> return (idle)
//   state 3  -> FUN_8007a624 (despawn)
//
// Ownership model (identical to the FUN_739ac handler/73cd8/741dc): CONTROL FLOW + node/global memory writes owned
// native; every sub-behavior CALL stays a reachable PSX leaf via rec_dispatch (NO recursion into them).
// NO GTE, NO render packets here. RE'd 1:1 from disas 0x8012EB54..0x8012ED80 (jump table 0x80109dec =
// {ec08,ec18,ec3c,ec4c,ec5c,ec6c}, decoded from the field RAM dump). It WRITES guest node state the
// still-recomp content reads -> content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B vs
// rec_super_call). The idle/active field path is exercised by the gate; the input/scene-driven sub-states
// are faithfully transcribed and verify when a scene drives them.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012EB54u;

}  // namespace

void beh_substate_edge_orchestrator(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t st = c->mem_r8(obj + 4);                    // s0 = node[4] (state)   [0x8012EB68]

  // ---- state dispatch [0x8012EB70..0x8012EBA8] ----
  if (st != 1) {
    if (st >= 2) {                                    // slti s0,2 / beqz [0x8012EB74..78]
      if (st == 2) return;                            // beq s0,2 -> epilogue   [0x8012EB94]
      if (st == 3) {                                  // beq s0,3 -> despawn     [0x8012EB9C]
        eng(c).spawn.despawn(obj);  // FUN_8007a624           [0x8012ED68]
      }
      return;                                         // other (>=2) -> epilogue [0x8012EBA4]
    }
    if (st != 0) return;                              // beqz s0 [0x8012EB80]; the s0<2 && !=0 path j's ED70
    // ---- STATE 0 [0x8012EBAC] ----
    c->r[4] = obj; rec_dispatch(c, 0x8012ED84u);      // FUN_8012ed84 (per-type init)
    return;                                           // j 0x8012ED70 (epilogue)
  }

  // ---- STATE 1 [0x8012EBBC] ----
  c->r[4] = obj; rec_dispatch(c, 0x80130AC4u);        // FUN_80130ac4
  if (c->r[2] != 0) {                                 // beqz v0 -> 0x8012EBD8 [0x8012EBC4]
    c->mem_w8(obj + 1, st);                           // sb s0,1(s1): node[1] = state (==1) [0x8012EBCC]
    eng(c).cull.enqueueVisibleClass4(obj);         // FUN_80077EBC — Cull::enqueueVisibleClass4  [0x8012EBD0]
  }

  uint8_t sub = c->mem_r8(obj + 5);                   // lbu v1,5(s1): node[5] [0x8012EBD8]
  if (sub < 6) {                                      // sltiu v0,v1,6 / beqz -> tail [0x8012EBE0..E4]
    // jump table 0x80109dec, node[5]*4 -> {ec08,ec18,ec3c,ec4c,ec5c,ec6c}
    switch (sub) {
      case 0:                                         // jt[0]=0x8012ec08
        c->r[4] = obj; rec_dispatch(c, 0x8012F494u);  // FUN_8012f494
        break;                                        // j 0x8012EC74 (tail)
      case 1:                                         // jt[1]=0x8012ec18
        if (c->mem_r8(0x1F800137u) == 0) {            // lbu v0,0x137(0x1f80_0000); bnez -> tail
          c->r[4] = obj; rec_dispatch(c, 0x8012F5B4u);// FUN_8012f5b4
        }
        break;                                        // j 0x8012EC74 (tail)
      case 2:                                         // jt[2]=0x8012ec3c
        c->r[4] = obj; rec_dispatch(c, 0x8012FD88u);  // FUN_8012fd88
        break;
      case 3:                                         // jt[3]=0x8012ec4c
        c->r[4] = obj; rec_dispatch(c, 0x80130524u);  // FUN_80130524
        break;
      case 4:                                         // jt[4]=0x8012ec5c
        c->r[4] = obj; rec_dispatch(c, 0x801313C4u);  // FUN_801313c4
        break;
      case 5:                                         // jt[5]=0x8012ec6c (NO j; falls through to tail)
        c->r[4] = obj; rec_dispatch(c, 0x8018C820u);  // FUN_8018c820
        break;
    }
  }

  // ---- tail [0x8012EC74] ----
  c->r[4] = obj; rec_dispatch(c, 0x80131134u);        // FUN_80131134
  c->r[4] = obj; rec_dispatch(c, 0x801316CCu);        // FUN_801316cc

  uint8_t g = c->mem_r8(0x800BF9D2u);                 // lbu v1,-0x62e(0x800c_0000) [0x8012EC88]
  // edge machine on node[0x78] keyed by g (DAT_800bf9d2); sets node[5]=4 on the 0/0xff edges.
  bool set5 = false;
  if (g == 0) {                                       // bnez v1 -> ECB4 [0x8012EC90]
    // v0 preset to 5 (delay slots at ECA8/ECB0) [0x8012EC98]
    if ((c->mem_r16(obj + 0x78) & 1) == 0) {          // andi 1; bnez -> ed2c
      c->mem_w16(obj + 0x78, 5);                      // sh v0(=5),0x78(s1) [delay slot @ECB0]
      set5 = true;                                    // j 0x8012ECD4
    }
  } else if (g == 0xff) {                             // bne v1,0xff -> ECE4 [0x8012ECB4]
    if ((c->mem_r16(obj + 0x78) & 1) == 0) {          // andi 1; bnez -> ed2c [0x8012ECC4]
      c->mem_w16(obj + 0x78, 0x8005);                 // addiu v0,-0x7ffb -> 0xffff8005; sh -> 0x8005 [0x8012ECD0]
      set5 = true;                                    // fall to ECD4
    }
  } else {                                            // g != 0 && g != 0xff [0x8012ECE4]
    uint16_t n78 = c->mem_r16(obj + 0x78);            // lhu v1,0x78(s1)
    if (n78 & 1) {                                    // andi 1; beqz -> ed2c
      uint16_t nv = (n78 & 0x8000) ? 0x8002 : 2;      // bnez (v1&0x8000) -> 0x8002 else 2 [0x8012ECF8/D00]
      c->mem_w16(obj + 0x78, nv);                     // sh v0,0x78(s1) [0x8012ED04]
      // node[0x48] -> node[0x4e] sign clamp [0x8012ED08..28]
      int16_t x = c->mem_r16s(obj + 0x48);    // lh v0,0x48(s1)
      if (x > 0)      c->mem_w16(obj + 0x4e, (uint16_t)(uint32_t)(int32_t)-0x80);  // blez false -> -0x80
      else if (x < 0) c->mem_w16(obj + 0x4e, 0x80);   // bgez false (x<0) -> +0x80
      // x == 0: node[0x4e] unchanged (bgez true -> goto ed2c)
    }
  }
  if (set5) {                                         // ECD4: node[5]=4, node[6]=0 [0x8012ECD4..E0]
    c->mem_w8(obj + 5, 4);                            // sb v0(=4),5(s1)
    c->mem_w8(obj + 6, 0);                            // sb zero,6(s1) [delay slot @ECE0]
  }

  // ---- common after-tail [0x8012ED2C] ----
  if (c->mem_r8(obj + 3) == 0) {                      // lbu v0,3(s1); bnez -> ed48 [0x8012ED34]
    uint32_t a1 = c->mem_r32(obj + 0xc8);             // lw a1,0xc8(s1) [0x8012ED3C]
    c->r[4] = obj; c->r[5] = a1; rec_dispatch(c, 0x80146348u);  // FUN_80146348 [0x8012ED40]
  }
  if (c->mem_r8(obj + 1) != 0) {                      // lbu v0,1(s1); beqz -> ed60 [0x8012ED50]
    c->r[4] = obj; rec_dispatch(c, 0x8012E8A8u);      // FUN_8012e8a8 [0x8012ED58]
  }
  c->mem_w8(obj + 0x2b, 0);                           // sb zero,0x2b(s1) [delay slot @ED64, always runs]
}
