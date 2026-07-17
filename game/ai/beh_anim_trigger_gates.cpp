// game/ai/beh_anim_trigger_gates.cpp — PC-native per-object BEHAVIOR handler FUN_80129C00.
//
// Overlay handler (~x2334/field-frame on seaside; ~130 instr), prologue 0x80129C00; `jr ra` at
// 0x80129E84. Disassembled from scratch/ram/field_seaside.bin incl. its in-overlay jump table
// (jt @0x80109C5C, 5 entries indexed by node[3]). Two-level state machine (outer node[4]):
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   >=4 : nothing.
//   STATE 0 : node[3] -> 0/1/2 FUN_801296E0, 3 FUN_8012982C, 4 FUN_80129984, >=5 nothing.
//   STATE 1 : dispatch jt[node[3]] (node[3]<5):
//       case 0/1: animation-trigger gates on area bytes 0x800E7EAA/0x800E7FC7 (and 0x800E7EB6 hword for
//                 case 0); on success node[1]=1, node[0xC0][+12] += 16, FUN_800517F8(node); case 0 also
//                 may call FUN_8007778C(node).
//       case 2/3: FUN_80129160 / FUN_801292E4(node).
//       case 4  : if area byte 0x800BF816==0 or 0x800BF8BC==255 -> node[4]=3; else (when node[5]==0 &&
//                 node[8]!=0) bump each of the node[8] records at node[0xC0+i*4] by +16 on field[+8] when
//                 it is < -127; then node[1]=1, FUN_80051C8C(node).
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf, a0=node). NO GTE/render.
// Transcribed 1:1 as a register machine (locals = guest regs, goto labels = guest addresses); the guest
// jump table becomes switch->goto; signed hword tests (lh/slti) preserved. The byte-exact A/B gate (full
// RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
#include "guest_abi.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80129C00u;

}  // namespace

void beh_anim_trigger_gates(Core* c) {
  uint32_t obj = c->r[4];                         // s0 = a0 (node)
  uint32_t v1;

  uint8_t st = c->mem_r8(obj + 4);                // node[4] = outer state
  if (getenv("PSXPORT_ANIMTG_ENTRY"))
    fprintf(stderr, "[animtg-entry] core=%p node=%08X st=%u n3=%u stage=%08X\n",
            (void*)c, obj, st, c->mem_r8(obj + 3), c->mem_r32(0x801fe00c));
  if (st == 1) goto Lcb0;
  if (st < 2) { if (st == 0) goto Lc50; goto Lret; }   // st<2 -> only st==0
  if (st == 2) goto Lret;
  if (st == 3) { eng(c).spawn.despawn(obj); goto Lret; }
  goto Lret;                                       // st >= 4 default

 Lc50:                                            // STATE 0 — node[3] dispatch (per decomp FUN_80129c00
                                                  // + recomp ov_a00_gen_80129C00). Prior version had
                                                  // `v1 == 2 -> FUN_8012982C` which is a case-swap bug —
                                                  // 4-record-alloc init runs on the WRONG node[3] value,
                                                  // producing +8 extra allocations at 0x800ED098 vs the
                                                  // recomp path. Corrected: n3==3 dispatches 0x8012982C.
  v1 = c->mem_r8(obj + 3);
  if (v1 == 3) { guest_leaf(c, 0x8012982Cu, obj); goto Lret; }
  if (v1 < 4) { if ((int8_t)v1 < 0) goto Lret; guest_leaf(c, 0x801296E0u, obj); goto Lret; }  // 0,1,2
  if (v1 == 4) { guest_leaf(c, 0x80129984u, obj); goto Lret; }
  goto Lret;                                       // node[3] >= 5

 Lcb0:                                            // STATE 1 — jump table on node[3]
  v1 = c->mem_r8(obj + 3);
  if (!(v1 < 5)) goto Lret;
  switch (v1) {
    case 0: goto Lce0;
    case 1: goto Ld60;
    case 2: guest_leaf(c, 0x80129160u, obj); goto Lret;   // case 2 @ db4
    case 3: guest_leaf(c, 0x801292E4u, obj); goto Lret;   // case 3 @ dc4
    default: goto Ldd4;                              // case 4
  }

 Lce0:                                            // STATE 1 / node[3]==0
  {
    uint8_t val = c->mem_r8(0x800E7EAAu);
    if (val == 31) {
      if (c->mem_r16s(0x800E7EB6u) < 8710) c->mem_w8(obj + 1, 1);
      goto Ld44;
    }
    if ((uint8_t)(val - 26) < 5) {                 // (val-26)&0xff < 5  ->  val in [26,30]
      uint8_t b = c->mem_r8(0x800E7FC7u);
      if (b == 1) { c->mem_w8(obj + 1, b); goto Ld44; }
    }
  }
 Ld3c:
  guest_leaf(c, 0x8007778Cu, obj);
 Ld44:
  if (c->mem_r8(obj + 1) == 0) goto Lret;
  goto Ld98;

 Ld60:                                            // STATE 1 / node[3]==1
  {
    uint8_t val = c->mem_r8(0x800E7EAAu);
    if (!((uint32_t)(val - 23) < 5)) goto Lret;    // val in [23,27]
    uint8_t b = c->mem_r8(0x800E7FC7u);
    if (b != 1) goto Lret;
    c->mem_w8(obj + 1, b);
  }
 Ld98:
  {
    uint32_t rec = c->mem_r32(obj + 0xC0);
    c->mem_w16(rec + 12, (uint16_t)(c->mem_r16(rec + 12) + 16));
    c->r[4] = obj; eng(c).graphicsBind.renderUpdate();
  }
  goto Lret;

 Ldd4:                                            // STATE 1 / node[3]==4
  if (c->mem_r8(0x800BF816u) == 0) { c->mem_w8(obj + 4, 3); goto Lret; }
  if (c->mem_r8(0x800BF8BCu) == 255) { c->mem_w8(obj + 4, 3); goto Lret; }
  if (c->mem_r8(obj + 5) == 0 && c->mem_r8(obj + 8) != 0) {
    uint32_t a2 = obj; int a0i = 0;
    do {
      uint32_t rec = c->mem_r32(a2 + 0xC0);
      if (c->mem_r16s(rec + 8) < -127)
        c->mem_w16(rec + 8, (uint16_t)(c->mem_r16(rec + 8) + 16));
      a0i += 1; a2 += 4;
    } while (a0i < (int)c->mem_r8(obj + 8));
  }
  c->mem_w8(obj + 1, 1);
  guest_leaf(c, 0x80051C8Cu, obj);
 Lret:
  return;
}
