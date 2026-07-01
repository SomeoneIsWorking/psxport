// engine/beh_rand_phase_cull.cpp — PC-native per-object BEHAVIOR handler FUN_8002918C.
//
// Resident handler (~46 lines), prologue 0x8002918C. Ported from the Ghidra decompile of FUN_8002918c
// (scratch/decomp/ram_f1000_all.c). A small PRNG phase-accumulator + cone-cull tick driven by the outer
// state node[4]:
//   STATE >3        : return (nothing).
//   STATE 2 or 3    : FUN_8007A624(node) (despawn/teardown leaf), return.
//   STATE !=0,!=1   : (state < 2 but not 0) return.
//   STATE 0 (init)  : node[4]=1; node[7]=0; node[0x34]=node[0x38] (32-bit copy);
//                     node[6] = (int8)(rand()>>0xB) + 8;  then if node[3]=='5' (0x35) AND
//                     DAT_800E7FFE (signed s16) < 0 -> node[6] = (int8)(rand()>>0xC) + 3.
//                     FALLS THROUGH into the common tail below.
//   COMMON TAIL (reached for STATE 1, and after STATE-0 init):
//                     node[7] += (int8)(rand()>>9);
//                     s = FUN_8002B278(node) (cone cull, low-16 result);
//                     if (int8)node[7] < 0 -> node[7] -= 0x80 (phase still pending; clear sign bit);
//                     else { node[0x34]=node[0x38]; if node[0x38]==0 -> node[4]=2;
//                            else if s==0 -> FUN_80031780(node) (list scan/reset). }
//
// CONTROL FLOW + the direct node WRITES owned native; every sub-behavior CALL stays reachable via
// rec_dispatch (pure-PSX leaf) — matching the EXACT established pattern of the sibling behavior ports:
// 0x8009A450 (PRNG), 0x8002B278 (cone cull) and 0x8007A624 are kept as rec_dispatch leaves exactly as
// every other engine/beh_*.cpp does (they advance shared RNG/cull state in guest RAM, and the A/B gate
// rolls RAM back before rec_super_call so both sides draw the same sequence). Signed widths preserved:
// node[6]/node[7] are signed char (int8); node[0x34]/node[0x38] are 32-bit; DAT_800E7FFE is a signed
// short (lh). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collision.h"  // ov_list_scan_31780 (FUN_80031780)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8002918Cu;

static inline void     leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline uint32_t call0(Core* c, uint32_t fn) { rec_dispatch(c, fn); return c->r[2]; }
static inline uint32_t call1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); return c->r[2]; }

void beh_rand_phase_cull(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t  st = c->mem_r8(nd + 4);              // bVar1 = node[4] (unsigned byte)

  if (st != 1) {
    if (st > 1) {                                // 1 < bVar1
      if (st > 3) return;                        // 3 < bVar1 -> nothing
      leaf1(c, nd, 0x8007a624u);                 // STATE 2/3: FUN_8007A624(node)
      return;
    }
    if (st != 0) return;                         // st < 2, not 1, not 0 -> nothing
    // ---- STATE 0: init ----
    c->mem_w8(nd + 4, 1);
    c->mem_w8(nd + 7, 0);
    c->mem_w32(nd + 0x34, c->mem_r32(nd + 0x38));            // 32-bit copy
    int32_t r = (int32_t)call0(c, 0x8009a450u);             // rand() draw
    c->mem_w8(nd + 6, (uint8_t)((int8_t)(int32_t)(r >> 0xb) + 8));
    if (c->mem_r8(nd + 3) == 0x35u && (int16_t)c->mem_r16(0x800E7FFEu) < 0) {  // node[3]=='5' && DAT_800E7FFE < 0
      int32_t r2 = (int32_t)call0(c, 0x8009a450u);          // rand() draw
      c->mem_w8(nd + 6, (uint8_t)((int8_t)(int32_t)(r2 >> 0xc) + 3));
    }
    // FALLTHROUGH into the common tail
  }

  // ---- COMMON TAIL (STATE 1, or after STATE-0 init) ----
  int32_t r3 = (int32_t)call0(c, 0x8009a450u);              // rand() draw
  c->mem_w8(nd + 7, (uint8_t)((int8_t)c->mem_r8(nd + 7) + (int8_t)(int32_t)(r3 >> 9)));
  int16_t cull = (int16_t)call1(c, nd, 0x8002b278u);        // sVar2 = FUN_8002B278(node) (short)
  if ((int8_t)c->mem_r8(nd + 7) < 0) {
    c->mem_w8(nd + 7, (uint8_t)(c->mem_r8(nd + 7) - 0x80));  // (int8)node[7] += -0x80  == clear sign bit
  } else {
    c->mem_w32(nd + 0x34, c->mem_r32(nd + 0x38));
    if (c->mem_r32(nd + 0x38) == 0) {
      c->mem_w8(nd + 4, 2);
    } else if (cull == 0) {
      c->r[4] = nd; ov_list_scan_31780(c);                  // FUN_80031780(node) — native
    }
  }
}

void ov_beh_rand_phase_cull(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("rand_phase_cullverify") ? 1 : 0;
  if (!s_v) { beh_rand_phase_cull(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_rand_phase_cull(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[rand_phase_cullverify] MISMATCH obj=%08x st=%u n3=%u n7=%d ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 3), (int8_t)c->mem_r8(obj + 7), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[rand_phase_cullverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_rand_phase_cull_run(Core* c) { ov_beh_rand_phase_cull(c); }
