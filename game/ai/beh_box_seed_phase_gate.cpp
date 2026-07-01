// engine/beh_box_seed_phase_gate.cpp — PC-native per-object BEHAVIOR handler FUN_8012A0B8.
//
// Overlay handler (~x2334/field-frame on seaside; ~135 instr), prologue 0x8012A0B8; `jr ra` at
// 0x8012A2D0. Disassembled from scratch/ram/field_seaside.bin. Two-level state machine (outer
// node[4] = s0):
//   STATE 2/3 : FUN_8007A624(node).   STATE >=4 : nothing.
//   STATE 0   : INIT — node[11]=32, node[4]=1, node[8]=0, node[9]=0, node[0x18]=0x8013EA64; then
//               if node[3]<2: FUN_801360F4(node,node[3]) + FUN_80139838(node,0)/(node,1);
//               else        : FUN_8013AC34(node,node[3]);
//               then copy a per-node[3] record from table @0x80149EC4 (stride 10) into the node:
//                 node[0x80]=tbl[+2] node[0x82]=tbl[+6] node[0x84]=tbl[+4] node[0x86]=tbl[+8]
//                 node[0x2E]=node[0x4E]=(tbl[+2]+tbl[+6])/2   (round-toward-zero)
//                 node[0x32]=node[0x50]=node[0x52]=tbl[+0]
//                 node[0x36]=(tbl[+4]+tbl[+8])/2
//               then FUN_80129E8C(node, node[0x32]).
//   STATE 1   : read scratchpad byte 0x1F800207; if 25 <= b < 32 -> FUN_80129E8C(node,...) and node[1]=1.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf, a0/a1 set as the guest does).
// The per-node[3] table @0x80149EC4 lives in the resident overlay RAM — we READ it live (same memory
// the recomp reads); it is NOT embedded. Transcribed 1:1 as a register machine (locals = guest regs,
// goto labels = guest addresses); signed (lh/sra) vs unsigned (lhu/lbu) preserved exactly. The
// byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012A0B8u;

static inline void leaf(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline void leaf2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}

void beh_box_seed_phase_gate(Core* c) {
  uint32_t s1 = c->r[4];                            // s1 = a0 (node)
  uint32_t s0 = c->mem_r8(s1 + 4);                  // s0 = node[4] = outer state

  if (s0 == 1) goto La28c;                          // STATE 1
  if ((int32_t)s0 < 2) { if (s0 == 0) goto La108; goto Lret; }  // s0<2 -> only s0==0
  if ((int32_t)s0 < 4) goto La2bc;                  // s0 in {2,3} -> FUN_8007A624
  goto Lret;                                        // s0 >= 4

 La108:                                             // STATE 0 — INIT
  {
    c->mem_w8(s1 + 11, 32);                         // node[11] = 32 (v0=32 from delay slot)
    c->mem_w8(s1 + 4, 1);                           // node[4] = 1
    uint8_t n3 = c->mem_r8(s1 + 3);                 // v1 = node[3]
    c->mem_w8(s1 + 8, 0);                           // node[8] = 0
    c->mem_w8(s1 + 9, 0);                           // node[9] = 0
    c->mem_w32(s1 + 0x18, 0x8013EA64u);             // node[0x18] = 0x8013EA64 (branch-delay store)

    if (n3 < 2) {
      leaf2(c, s1, n3, 0x801360F4u);                // FUN_801360F4(node, node[3])
      for (int i = 0; i < 2; i++)
        leaf2(c, s1, (uint32_t)i, 0x80139838u);     // FUN_80139838(node, 0), (node, 1)
    } else {
      leaf2(c, s1, n3, 0x8013AC34u);                // FUN_8013AC34(node, node[3])
    }

    // per-node[3] record copy: table @0x80149EC4, stride 10 bytes, element = base + node[3]*10
    uint32_t elem = 0x80149EC4u + (uint32_t)n3 * 10u;
    c->mem_w16(s1 + 0x80, c->mem_r16(elem + 2));    // node[0x80] = tbl[+2]
    c->mem_w16(s1 + 0x82, c->mem_r16(elem + 6));    // node[0x82] = tbl[+6]
    c->mem_w16(s1 + 0x84, c->mem_r16(elem + 4));    // node[0x84] = tbl[+4]
    c->mem_w16(s1 + 0x86, c->mem_r16(elem + 8));    // node[0x86] = tbl[+8]

    // node[0x2E] = node[0x4E] = (tbl[+2] + tbl[+6]) / 2  (signed, round toward zero)
    int32_t s_26 = (int16_t)c->mem_r16(elem + 2) + (int16_t)c->mem_r16(elem + 6);
    int32_t avg1 = (int32_t)(((uint32_t)s_26 + ((uint32_t)s_26 >> 31)) >> 1);  // sra((x + signbit),1)
    c->mem_w16(s1 + 0x2E, (uint16_t)avg1);
    c->mem_w16(s1 + 0x32, c->mem_r16(elem + 0));    // node[0x32] = tbl[+0]
    c->mem_w16(s1 + 0x4E, c->mem_r16(s1 + 0x2E));   // node[0x4E] = node[0x2E]

    // node[0x36] = (tbl[+4] + tbl[+8]) / 2  (signed, round toward zero)
    int32_t s_36 = (int16_t)c->mem_r16(elem + 4) + (int16_t)c->mem_r16(elem + 8);
    int32_t avg2 = (int32_t)(((uint32_t)s_36 + ((uint32_t)s_36 >> 31)) >> 1);
    uint16_t v32 = c->mem_r16(s1 + 0x32);           // node[0x32]
    c->mem_w16(s1 + 0x36, (uint16_t)avg2);
    c->mem_w16(s1 + 0x50, v32);                     // node[0x50] = node[0x32]
    c->mem_w16(s1 + 0x52, v32);                     // node[0x52] = node[0x32]  (delay slot store)
    leaf2(c, s1, (uint32_t)v32, 0x80129E8Cu);       // FUN_80129E8C(node, node[0x32])
  }
  goto Lret;

 La28c:                                             // STATE 1
  {
    uint8_t b = c->mem_r8(0x1F800207u);             // scratchpad byte 0x1F800207
    if (b >= 32) goto Lret;
    if (b < 25) goto Lret;
    leaf(c, s1, 0x80129E8Cu);                       // FUN_80129E8C(node)  (a1 = leftover, untouched)
    c->mem_w8(s1 + 1, (uint8_t)s0);                 // node[1] = s0 (=1, delay-slot store)
  }
  goto Lret;

 La2bc:
  leaf(c, s1, 0x8007A624u);                         // FUN_8007A624(node)
 Lret:
  return;
}

void ov_beh_box_seed_phase_gate(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("box_seed_phase_gateverify") ? 1 : 0;
  if (!s_v) { beh_box_seed_phase_gate(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_box_seed_phase_gate(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[box_seed_phase_gateverify] MISMATCH obj=%08x st=%u n3=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 3), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[box_seed_phase_gateverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_box_seed_phase_gate_run(Core* c) { ov_beh_box_seed_phase_gate(c); }
