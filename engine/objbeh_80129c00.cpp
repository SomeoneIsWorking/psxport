// engine/objbeh_80129c00.cpp — PC-native per-object BEHAVIOR handler FUN_80129C00.
//
// Overlay handler (~x2334/field-frame on seaside; ~130 instr), prologue 0x80129C00; `jr ra` at
// 0x80129E84. Disassembled from scratch/ram/field_seaside.bin incl. its in-overlay jump table
// (jt @0x80109C5C, 5 entries indexed by node[3]). Two-level state machine (outer node[4]):
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   >=4 : nothing.
//   STATE 0 : node[3] -> 0/1/3 FUN_801296E0, 2 FUN_8012982C, 4 FUN_80129984, >=5 nothing.
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
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80129C00u;

static inline void leaf(Core* c, uint32_t obj, uint32_t fn) { c->r[4] = obj; rec_dispatch(c, fn); }

void beh_80129c00(Core* c) {
  uint32_t obj = c->r[4];                         // s0 = a0 (node)
  uint32_t v1;

  uint8_t st = c->mem_r8(obj + 4);                // node[4] = outer state
  if (st == 1) goto Lcb0;
  if (st < 2) { if (st == 0) goto Lc50; goto Lret; }   // st<2 -> only st==0
  if (st == 2) goto Lret;
  if (st == 3) { leaf(c, obj, 0x8007A624u); goto Lret; }
  goto Lret;                                       // st >= 4 default

 Lc50:                                            // STATE 0 — node[3] dispatch
  v1 = c->mem_r8(obj + 3);
  if (v1 == 2) { leaf(c, obj, 0x8012982Cu); goto Lret; }
  if (v1 < 4) { if ((int8_t)v1 < 0) goto Lret; leaf(c, obj, 0x801296E0u); goto Lret; }  // 0,1,3
  if (v1 == 4) { leaf(c, obj, 0x80129984u); goto Lret; }
  goto Lret;                                       // node[3] >= 5

 Lcb0:                                            // STATE 1 — jump table on node[3]
  v1 = c->mem_r8(obj + 3);
  if (!(v1 < 5)) goto Lret;
  switch (v1) {
    case 0: goto Lce0;
    case 1: goto Ld60;
    case 2: leaf(c, obj, 0x80129160u); goto Lret;   // case 2 @ db4
    case 3: leaf(c, obj, 0x801292E4u); goto Lret;   // case 3 @ dc4
    default: goto Ldd4;                              // case 4
  }

 Lce0:                                            // STATE 1 / node[3]==0
  {
    uint8_t val = c->mem_r8(0x800E7EAAu);
    if (val == 31) {
      if ((int16_t)c->mem_r16(0x800E7EB6u) < 8710) c->mem_w8(obj + 1, 1);
      goto Ld44;
    }
    if ((uint8_t)(val - 26) < 5) {                 // (val-26)&0xff < 5  ->  val in [26,30]
      uint8_t b = c->mem_r8(0x800E7FC7u);
      if (b == 1) { c->mem_w8(obj + 1, b); goto Ld44; }
    }
  }
 Ld3c:
  leaf(c, obj, 0x8007778Cu);
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
    leaf(c, obj, 0x800517F8u);
  }
  goto Lret;

 Ldd4:                                            // STATE 1 / node[3]==4
  if (c->mem_r8(0x800BF816u) == 0) { c->mem_w8(obj + 4, 3); goto Lret; }
  if (c->mem_r8(0x800BF8BCu) == 255) { c->mem_w8(obj + 4, 3); goto Lret; }
  if (c->mem_r8(obj + 5) == 0 && c->mem_r8(obj + 8) != 0) {
    uint32_t a2 = obj; int a0i = 0;
    do {
      uint32_t rec = c->mem_r32(a2 + 0xC0);
      if ((int16_t)c->mem_r16(rec + 8) < -127)
        c->mem_w16(rec + 8, (uint16_t)(c->mem_r16(rec + 8) + 16));
      a0i += 1; a2 += 4;
    } while (a0i < (int)c->mem_r8(obj + 8));
  }
  c->mem_w8(obj + 1, 1);
  leaf(c, obj, 0x80051C8Cu);
 Lret:
  return;
}

void ov_beh_80129c00(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("obj80129c00verify") ? 1 : 0;
  if (!s_v) { beh_80129c00(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_80129c00(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[obj80129c00verify] MISMATCH obj=%08x st=%u n3=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 3), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[obj80129c00verify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_80129c00_run(Core* c) { ov_beh_80129c00(c); }
