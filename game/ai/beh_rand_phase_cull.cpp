// game/ai/beh_rand_phase_cull.cpp — PC-native per-object BEHAVIOR handler FUN_8002918C.
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
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "collision.h"  // Collision::listScan (FUN_80031780)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8002918Cu;

static inline void     leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline uint32_t call0(Core* c, uint32_t fn) { rec_dispatch(c, fn); return c->r[2]; }
static inline uint32_t call1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); return c->r[2]; }

}  // namespace

void beh_rand_phase_cull(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t  st = c->mem_r8(nd + 4);              // bVar1 = node[4] (unsigned byte)

  if (st != 1) {
    if (st > 1) {                                // 1 < bVar1
      if (st > 3) return;                        // 3 < bVar1 -> nothing
      eng(c).spawn.despawn(nd);                 // STATE 2/3: FUN_8007A624(node)
      return;
    }
    if (st != 0) return;                         // st < 2, not 1, not 0 -> nothing
    // ---- STATE 0: init ----
    c->mem_w8(nd + 4, 1);
    c->mem_w8(nd + 7, 0);
    c->mem_w32(nd + 0x34, c->mem_r32(nd + 0x38));            // 32-bit copy
    int32_t r = (int32_t)call0(c, 0x8009a450u);             // rand() draw
    c->mem_w8(nd + 6, (uint8_t)((int8_t)(int32_t)(r >> 0xb) + 8));
    if (c->mem_r8(nd + 3) == 0x35u && c->mem_r16s(0x800E7FFEu) < 0) {  // node[3]=='5' && DAT_800E7FFE < 0
      int32_t r2 = (int32_t)call0(c, 0x8009a450u);          // rand() draw
      c->mem_w8(nd + 6, (uint8_t)((int8_t)(int32_t)(r2 >> 0xc) + 3));
    }
    // FALLTHROUGH into the common tail
  }

  // ---- COMMON TAIL (STATE 1, or after STATE-0 init) ----
  int32_t r3 = (int32_t)call0(c, 0x8009a450u);              // rand() draw
  c->mem_w8(nd + 7, (uint8_t)(c->mem_r8s(nd + 7) + (int8_t)(int32_t)(r3 >> 9)));
  int16_t cull = (int16_t)call1(c, nd, 0x8002b278u);        // sVar2 = FUN_8002B278(node) (short)
  if (c->mem_r8s(nd + 7) < 0) {
    c->mem_w8(nd + 7, (uint8_t)(c->mem_r8(nd + 7) - 0x80));  // (int8)node[7] += -0x80  == clear sign bit
  } else {
    c->mem_w32(nd + 0x34, c->mem_r32(nd + 0x38));
    if (c->mem_r32(nd + 0x38) == 0) {
      c->mem_w8(nd + 4, 2);
    } else if (cull == 0) {
      eng(c).collision.listScan(nd);                     // FUN_80031780(node) — native
    }
  }
}
