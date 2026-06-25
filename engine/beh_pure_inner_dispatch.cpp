// engine/beh_pure_inner_dispatch.cpp — PC-native per-object BEHAVIOR handler FUN_80136D9C.
//
// Overlay handler (~x2334/field-frame on seaside; ~90 instr), prologue 0x80136D9C; `jr ra` at
// 0x80136F00. Disassembled from scratch/ram/field_seaside.bin. A pure CONTROL-FLOW dispatcher: it
// owns NO node writes of its own — every effect happens inside the sub-behavior leaves it routes to.
// Two-level dispatch (outer node[4], inner node[5]):
//   STATE 0 : FUN_80136F08(node).        STATE 2 : nothing.       STATE 3 : FUN_8007A624(node).  >=4: nothing.
//   STATE 1 : unless (byte 0x800BF89C != 2 && node[3]==2 && byte 0x800E7EAA==1), call FUN_8007778C(node);
//             then inner node[5]: 0 -> node[3]==3 ? FUN_8018CDC4 : FUN_80138A64; 1 -> byte 0x800BF809==0 ?
//             FUN_80137198 : (skip); 2 -> FUN_8018CA1C; then if node[1]!=0 -> FUN_801389C8(node).
//
// Ownership model: CONTROL FLOW owned native; all 8 sub-behavior CALLs stay reachable via rec_dispatch
// (pure-PSX leaf, a0=node). NO GTE/render. Transcribed 1:1 as a register machine (locals = guest regs,
// goto labels = guest addresses). The handler writes no RAM, so the A/B gate (full RAM+scratchpad vs
// rec_super_call) only confirms the routing chose the same leaves in the same order.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80136D9Cu;

static inline void leaf(Core* c, uint32_t obj, uint32_t fn) { c->r[4] = obj; rec_dispatch(c, fn); }

void beh_pure_inner_dispatch(Core* c) {
  uint32_t obj = c->r[4];                         // s0 = a0 (node)

  uint8_t st = c->mem_r8(obj + 4);                // node[4] = outer state
  if (st == 1) goto Le00;
  if (st < 2) { if (st == 0) goto Ldf0; goto Lret; }   // st<2 -> only st==0 reachable
  if (st == 2) goto Lret;
  if (st == 3) goto Lef0;
  goto Lret;                                       // st >= 4 default

 Lef0:  leaf(c, obj, 0x8007A624u); goto Lret;      // STATE 3
 Ldf0:  leaf(c, obj, 0x80136F08u); goto Lret;      // STATE 0

 Le00:                                             // STATE 1
  // call FUN_8007778C unless (0x800BF89C != 2 && node[3]==2 && 0x800E7EAA==1)
  if (c->mem_r8(0x800BF89Cu) != 2 && c->mem_r8(obj + 3) == 2 && c->mem_r8(0x800E7EAAu) == 1)
    goto Le3c;
  leaf(c, obj, 0x8007778Cu);
 Le3c:
  {
    uint8_t n5 = c->mem_r8(obj + 5);              // inner state
    if (n5 == 1) goto Lea4;
    if (n5 >= 2) { if (n5 == 2) goto Lec8; goto Led0; }
    // n5 == 0
    if (c->mem_r8(obj + 3) == 3) leaf(c, obj, 0x8018CDC4u);
    else                         leaf(c, obj, 0x80138A64u);
    goto Led0;
  }
 Lea4:                                             // n5 == 1
  if (c->mem_r8(0x800BF809u) == 0) leaf(c, obj, 0x80137198u);
  goto Led0;
 Lec8:                                             // n5 == 2
  leaf(c, obj, 0x8018CA1Cu);
  // fall into Led0
 Led0:
  if (c->mem_r8(obj + 1) != 0) leaf(c, obj, 0x801389C8u);
 Lret:
  return;
}

void ov_beh_pure_inner_dispatch(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("pure_inner_dispatchverify") ? 1 : 0;
  if (!s_v) { beh_pure_inner_dispatch(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_pure_inner_dispatch(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[pure_inner_dispatchverify] MISMATCH obj=%08x st=%u n5=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[pure_inner_dispatchverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_pure_inner_dispatch_run(Core* c) { ov_beh_pure_inner_dispatch(c); }
