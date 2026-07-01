// engine/beh_area_event_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80071A3C.
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
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80071A3Cu;

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
      if (c->mem_r8(obj + 1) != 0) { c->r[4] = obj; rec_dispatch(c, 0x800518FCu); }
      break;
    case 3:
      world_despawn(c, obj);
      break;
    default:  // state 2 and any other: no-op
      break;
  }
}

void ov_beh_area_event_dispatch(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("area_event_dispatchverify") ? 1 : 0;
  if (!s_v) { beh_area_event_dispatch(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_area_event_dispatch(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[area_event_dispatchverify] MISMATCH obj=%08x st=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[area_event_dispatchverify] %ld matches\n", ng);
}

}  // namespace

// Exported entry — the engine's per-object dispatch calls THIS to run the owned behavior.
void ov_beh_area_event_dispatch_run(Core* c) { ov_beh_area_event_dispatch(c); }
