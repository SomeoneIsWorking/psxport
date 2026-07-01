// engine/beh_single_child_cull.cpp — PC-native per-object BEHAVIOR handler FUN_80132400.
//
// Overlay handler (~x778/field-frame on seaside; ~80 instr), prologue 0x80132400; `jr ra` at
// 0x80132540. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4]:
//   STATE 0 : INIT. v0 = FUN_80051B70(node, 12, 37); if v0!=0 -> return. Else seed node[0x80..0x86]=
//             30/60/50/100, node[0x60]=-2350, node[0x62]=-1630, node[0x50]=1920, node[0]=1,
//             node[0x29]=0, node[0x5E]=0, node[4]++, node[0x32]+=128, node[3]=0; then
//             node[0x10] = FUN_8013A730(node).
//   STATE 1 : if mem[0x800BF89C]==2 OR mem[0x800E7EAA]!=node[4]: v0=FUN_8007778C(node); if v0!=0 ->
//             FUN_80132020(node) + FUN_800517F8(node). ALWAYS node[0x2B]=0 at the tail.
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node/global WRITES owned
// native; every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). a0 fidelity: the
// guest sets a0=node before the STATE-0 FUN_80051B70 (delay-slot `addu a0,s0,zero`) and again before
// FUN_8013A730; in STATE 1, a0 is still the original node[4] byte (compared vs mem[0x800E7EAA]). The
// dead `addiu v1,v0,-1936` (= 0x800BF870, never stored/read) is dropped — no RAM effect. Transcribed
// 1:1 as a register machine; signed (sh) preserved. The byte-exact A/B gate (full RAM+scratchpad vs
// rec_super_call) is the safety net. NO GTE.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80132400u;

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}
static inline uint32_t leafr3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn); return c->r[2];
}

void beh_single_child_cull(Core* c) {
  uint32_t nd = c->r[4];                          // s0 = a0 (node)
  uint32_t st = c->mem_r8(nd + 4);                // a0 = node[4] = outer state

  if (st == 1) goto S1;
  if ((int32_t)st < 2) { if (st == 0) goto S0; goto Lret; }
  if (st == 2) goto Lret;
  if (st == 3) { leaf1(c, nd, 0x8007a624u); goto Lret; }
  goto Lret;

 // ================= STATE 0 (INIT) =================
 S0: {
   if (leafr3(c, nd, 12, 37, 0x80051b70u) != 0) goto Lret;   // FUN_80051B70(node,12,37) -> bail if !=0
   c->mem_w16(nd + 0x80, 30);
   c->mem_w16(nd + 0x82, 60);
   c->mem_w16(nd + 0x84, 50);
   c->mem_w16(nd + 0x86, 100);
   c->mem_w16(nd + 0x60, (uint16_t)(int16_t)-2350);
   c->mem_w16(nd + 0x62, (uint16_t)(int16_t)-1630);
   c->mem_w16(nd + 0x50, 1920);
   uint32_t s = c->mem_r8(nd + 4);                 // node[4]
   uint32_t h = c->mem_r16(nd + 0x32);             // node[0x32]
   c->mem_w8(nd + 0, 1);
   c->mem_w8(nd + 0x29, 0);
   c->mem_w8(nd + 0x5e, 0);
   c->mem_w8(nd + 4, (uint8_t)(s + 1));            // node[4]++
   c->mem_w16(nd + 0x32, (uint16_t)(h + 128));     // node[0x32] += 128
   c->mem_w8(nd + 3, 0);
   c->mem_w32(nd + 0x10, leafr1(c, nd, 0x8013a730u));   // node[0x10] = FUN_8013A730(node)
   goto Lret;
 }

 // ================= STATE 1 =================
 S1: {
   bool work;
   if (c->mem_r8(0x800bf89cu) == 2) work = true;
   else work = (c->mem_r8(0x800e7eaau) != st);     // st = original node[4] byte
   if (work) {
     if (leafr1(c, nd, 0x8007778cu) != 0) {        // FUN_8007778C(node)
       leaf1(c, nd, 0x80132020u);                  // FUN_80132020(node)
       c->r[4] = nd; ov_obj_render_update(c);                  // FUN_800517F8(node)
     }
   }
   c->mem_w8(nd + 0x2b, 0);                         // node[0x2B] = 0 (every STATE-1 path)
   goto Lret;
 }

 Lret:
  return;
}

void ov_beh_single_child_cull(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("single_child_cullverify") ? 1 : 0;
  if (!s_v) { beh_single_child_cull(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_single_child_cull(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[single_child_cullverify] MISMATCH obj=%08x st=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[single_child_cullverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_single_child_cull_run(Core* c) { ov_beh_single_child_cull(c); }
