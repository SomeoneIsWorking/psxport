// engine/beh_two_child_steer.cpp — PC-native per-object BEHAVIOR handler FUN_80131D08.
//
// Overlay handler (~x778/field-frame on seaside; ~135 instr), prologue 0x80131D08; `jr ra` at
// 0x80131F2C. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT. If global mem[0x800ED098] < 2 -> node[4]=3 return. Else seed node[8]=node[9]=2,
//             node[0]=1, node[13]=node[11]=0, node[4]++; allocate 2 child records (FUN_8007AAE8) into
//             node[0xC0]/node[0xC4], each zeroed (rec[6]=-1, rec[0]=rec[4]=rec[8]=rec[12]=0); then
//             FUN_80051B04(rec0,12,2) and FUN_80051B04(rec1,12,3); rec1[0]=6, rec1[2]=-1400; seed
//             node[0x80..0x86]=50/100/1328/1328, node[0x4A]=1, node[0x40]=node[0x42]=node[0x48]=0,
//             node[0x4C]=3.
//   STATE 1 : per node[5]: ==1 -> FUN_80131840(node); ==0 -> node[5]=(mem[0x800BF8B8]==255 ? 2 : 1);
//             then a1 = sign16(scratch[0x162] - node[0x32]); FUN_800778E4(node, a1); if !=0 ->
//             rec1[10]=(rec1[10]-32)&0xFFF, FUN_800517F8(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/record/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). a0 fidelity in the
// record loop: the guest's a0 at the first FUN_8007AAE8 is the ORIGINAL state byte (node[4] read before
// the ++), and a0 is not rewritten between the two FUN_8007AAE8 calls — so set c->r[4]=orig-state and
// don't touch it across the loop. Transcribed 1:1 as a register machine; signed (lh/sra) vs unsigned
// preserved. The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80131D08u;

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline void leaf3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}
static inline uint32_t leafr2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn); return c->r[2];
}

void beh_two_child_steer(Core* c) {
  uint32_t nd = c->r[4];                          // s1 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                // a0 = node[4] = outer state

  if (st == 1) goto L_e74;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) { leaf1(c, nd, 0x8007a624u); goto Lret; }
  goto Lret;

 // ================= STATE 0 (INIT) =================
 S0: {
   if ((int16_t)c->mem_r16(0x800ed098u) < 2) { c->mem_w8(nd + 4, 3); goto Lret; }
   uint32_t orig = c->mem_r8(nd + 4);              // guest a0 for FUN_8007AAE8 (state before ++)
   c->mem_w8(nd + 8, 2);
   c->mem_w8(nd + 9, 2);
   c->mem_w8(nd + 0, 1);                            // node[0] = a1 = 1
   c->mem_w8(nd + 13, 0);
   c->mem_w8(nd + 11, 0);
   c->mem_w8(nd + 4, (uint8_t)(orig + 1));          // node[4]++
   c->r[4] = orig;                                  // mirror guest a0 for first FUN_8007AAE8
   uint32_t s0 = nd;
   int s2 = 0;
   do {
     rec_dispatch(c, 0x8007aae8u);                  // FUN_8007AAE8() -> v0 (alloc); a0 = guest a0
     uint32_t rec = c->r[2];
     s2 += 1;
     c->mem_w32(s0 + 0xc0, rec);
     c->mem_w16(rec + 6, (uint16_t)(int16_t)-1);
     c->mem_w32(rec + 0, 0);
     c->mem_w16(rec + 4, 0);
     c->mem_w32(rec + 8, 0);
     c->mem_w32(rec + 12, 0);
     s0 += 4;
   } while (s2 < 2);
   leaf3(c, c->mem_r32(nd + 0xc0), 12, 2, 0x80051b04u);   // FUN_80051B04(rec0, 12, 2)
   leaf3(c, c->mem_r32(nd + 0xc4), 12, 3, 0x80051b04u);   // FUN_80051B04(rec1, 12, 3)
   uint32_t rc1 = c->mem_r32(nd + 0xc4);
   c->mem_w16(rc1 + 0, 6);
   c->mem_w16(rc1 + 2, (uint16_t)(int16_t)-1400);
   c->mem_w16(nd + 0x80, 50);
   c->mem_w16(nd + 0x82, 100);
   c->mem_w16(nd + 0x84, 1328);
   c->mem_w16(nd + 0x86, 1328);
   c->mem_w16(nd + 0x4a, 1);
   c->mem_w16(nd + 0x40, 0);
   c->mem_w16(nd + 0x42, 0);
   c->mem_w16(nd + 0x48, 0);
   c->mem_w16(nd + 0x4c, 3);
   goto Lret;
 }

 // ================= STATE 1 =================
 L_e74: {
   uint8_t n5 = c->mem_r8(nd + 5);
   if (n5 == st) goto L_eb8;                        // st == 1 (a0)
   if ((int32_t)n5 < 2) {
     if (n5 != 0) goto Lec;                          // dead (n5==1 handled above)
     if (c->mem_r8(0x800bf8b8u) == 255) c->mem_w8(nd + 5, 2);
     else c->mem_w8(nd + 5, (uint8_t)st);            // node[5] = 1
     goto Lec;
   }
   goto Lec;                                         // n5 >= 2
 }
 L_eb8: {
   leaf1(c, nd, 0x80131840u);                        // FUN_80131840(node)
   goto Lec;
 }
 Lec: {
   uint16_t a = c->mem_r16(0x1f800162u);
   uint16_t b = c->mem_r16(nd + 0x32);
   uint32_t a1 = (uint32_t)(int32_t)(int16_t)(uint16_t)(a - b);
   if (leafr2(c, nd, a1, 0x800778e4u) == 0) goto Lret;   // FUN_800778E4(node, a1)
   uint32_t rc1 = c->mem_r32(nd + 0xc4);
   c->mem_w16(rc1 + 10, (uint16_t)((c->mem_r16(rc1 + 10) - 32) & 0x0fff));
   c->r[4] = nd; ov_obj_render_update(c);                        // FUN_800517F8(node)
   goto Lret;
 }

 Lret:
  return;
}

void ov_beh_two_child_steer(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("two_child_steerverify") ? 1 : 0;
  if (!s_v) { beh_two_child_steer(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_two_child_steer(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[two_child_steerverify] MISMATCH obj=%08x st=%u n5=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[two_child_steerverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_two_child_steer_run(Core* c) { ov_beh_two_child_steer(c); }
