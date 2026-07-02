// engine/beh_pure_substate_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80125E0C.
//
// Overlay handler (~x1556/field-frame on seaside; ~80 instr), prologue 0x80125E0C; `jr ra` at
// 0x80125F48. Disassembled from scratch/ram/field_seaside.bin. PURE control-flow dispatcher — it has
// NO direct node writes of its own; all effects are in the sub-behavior leaves. Outer state on node[4]:
//   STATE 0 : FUN_801253E8(node).
//   STATE 1 : FUN_8007778C(node); then node[5] -> 0/FUN_80125FE0, 1/FUN_801255CC, 2/FUN_80125800, else
//             none; then the common tail.
//   STATE 2 : FUN_8007778C(node); if node[5] in {2,3} FUN_801261FC(node); then the common tail.
//   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//   common tail (states 1/2): if node[1] != 0, FUN_800518FC(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW owned native; every sub-behavior CALL stays
// reachable via rec_dispatch (pure-PSX leaf, a0=node). Transcribed 1:1 as a register machine; the
// node[5] bltz test is preserved (node[5] is an lbu, so it can never be < 0 as a 32-bit signed value —
// a dead edge in the guest, kept for fidelity). The byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (c->engine.spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80125E0Cu;

static inline void leaf(Core* c, uint32_t obj, uint32_t fn) { c->r[4] = obj; rec_dispatch(c, fn); }

void beh_pure_substate_dispatch(Core* c) {
  uint32_t s1 = c->r[4];                            // s1 = a0 (node)
  uint32_t s0 = c->mem_r8(s1 + 4);                  // s0 = node[4] = outer state

  if (s0 == 1) goto S1;
  if ((int32_t)s0 < 2) { if (s0 == 0) { leaf(c, s1, 0x801253E8u); } goto Lret; }  // STATE 0
  if (s0 == 2) goto S2;
  if (s0 == 3) { c->engine.spawn.despawn(s1); goto Lret; }   // STATE 3
  goto Lret;                                        // s0 >= 4

 S1: {
    leaf(c, s1, 0x8007778Cu);                       // FUN_8007778C(node)
    uint8_t n5 = c->mem_r8(s1 + 5);                 // node[5]
    switch (n5) {
      case 0: leaf(c, s1, 0x80125FE0u); break;
      case 1: leaf(c, s1, 0x801255CCu); break;
      case 2: leaf(c, s1, 0x80125800u); break;
      default: break;                               // node[5] >= 3: none
    }
    goto Ltail;
  }

 S2: {
    leaf(c, s1, 0x8007778Cu);                       // FUN_8007778C(node)
    int32_t n5 = (int32_t)c->mem_r8(s1 + 5);        // node[5] (bltz test, always >= 0 for an lbu)
    if (n5 >= 0 && n5 >= 2 && n5 < 4)               // node[5] in {2,3}
      leaf(c, s1, 0x801261FCu);                     // FUN_801261FC(node)
    goto Ltail;
  }

 Ltail:                                             // common tail @0x80125f14
  if (c->mem_r8(s1 + 1) != 0)                       // node[1]
    leaf(c, s1, 0x800518FCu);                       // FUN_800518FC(node)
 Lret:
  return;
}

void ov_beh_pure_substate_dispatch(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("pure_substate_dispatchverify") ? 1 : 0;
  if (!s_v) { beh_pure_substate_dispatch(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_pure_substate_dispatch(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[pure_substate_dispatchverify] MISMATCH obj=%08x st=%u n5=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[pure_substate_dispatchverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_pure_substate_dispatch_run(Core* c) { ov_beh_pure_substate_dispatch(c); }
