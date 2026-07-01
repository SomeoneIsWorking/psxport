// engine/beh_box_rearm_sub.cpp — PC-native per-object BEHAVIOR handler FUN_8013ADBC.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8013ADBC
// (scratch/decomp/field2/8013adbc.c). Outer state machine on node[4]:
//   STATE 0 : INIT — seed the size/box params node[0x80]=0x50, node[0x82]=0xA0, node[0x84]=200,
//             node[0x86]=400, node[0x40]=30; node[4]=1, node[0]=1, node[0x0B]=node[0x2B]=node[0x29]=
//             node[0x5E]=0.
//   STATE 1 : if FUN_8007778C(node)!=0 -> FUN_8013AC98(node); then node[0x29]=node[0x2B]=0.
//   STATE 2 : if node[5]==0 re-arm (node[4]=1, node[0]=1, node[0x29]=1, node[5]=node[0x5E]); then
//             FUN_8013AC98(node).
//   STATE 3 : FUN_8007A624(node).
//
// CONTROL FLOW + the direct node WRITES owned native; the sub-behavior CALLs (FUN_8013AC98, FUN_8007778C,
// FUN_8007A624) stay reachable via rec_dispatch (pure-PSX leaves). Store widths from the decompile
// (undefined2 = 16-bit sh, byte = sb). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call)
// is the safety net.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013ADBCu;

static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}
static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }

void beh_box_rearm_sub(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    if (leafr1(c, nd, 0x8007778cu) != 0) leaf1(c, nd, 0x8013ac98u);   // FUN_8007778C / FUN_8013AC98
    c->mem_w8(nd + 0x29, 0);
    c->mem_w8(nd + 0x2b, 0);
  } else if (st < 2) {
    if (st == 0) {
      c->mem_w16(nd + 0x80, 0x50);
      c->mem_w16(nd + 0x82, 0xa0);
      c->mem_w16(nd + 0x84, 200);
      c->mem_w16(nd + 0x86, 400);
      c->mem_w8(nd + 4, 1);
      c->mem_w8(nd + 0, 1);
      c->mem_w8(nd + 0x0b, 0);
      c->mem_w8(nd + 0x2b, 0);
      c->mem_w8(nd + 0x29, 0);
      c->mem_w8(nd + 0x5e, 0);
      c->mem_w16(nd + 0x40, 0x1e);
    }
  } else if (st == 2) {
    if (c->mem_r8(nd + 5) == 0) {
      c->mem_w8(nd + 4, 1);
      c->mem_w8(nd + 0, 1);
      c->mem_w8(nd + 0x29, 1);
      c->mem_w8(nd + 5, c->mem_r8(nd + 0x5e));
    }
    leaf1(c, nd, 0x8013ac98u);                                        // FUN_8013AC98
  } else if (st == 3) {
    leaf1(c, nd, 0x8007a624u);                                        // FUN_8007A624
  }
}

void ov_beh_box_rearm_sub(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("box_rearm_subverify") ? 1 : 0;
  if (!s_v) { beh_box_rearm_sub(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_box_rearm_sub(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[box_rearm_subverify] MISMATCH obj=%08x st=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[box_rearm_subverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_box_rearm_sub_run(Core* c) { ov_beh_box_rearm_sub(c); }
