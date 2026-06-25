// engine/beh_typed_anim_spawn.cpp — PC-native per-object BEHAVIOR handler FUN_8012DA04.
//
// Overlay handler (~x2331/field-frame on seaside; ~200 instr), prologue 0x8012DA04; `jr ra` at
// 0x8012DE2C. Disassembled from scratch/ram/field_seaside.bin. Outer state machine on node[4] (v1):
//   STATE 0 : INIT — node[3]<8 dispatches in-overlay jump table jt0 @0x80109DAC; sets node[4]=1.
//       jt0 case 0/1/2/6/7 -> reset block (node[11]=16, node[8]=240, node[13]/0x5A/0x5C/0x47/0x60=0);
//       case 3/4 -> FUN_80051B70(node,12,79); on 0 set node[0xC0][8]=+182(n3==3)/-182(n3==4) +
//                   FUN_800517F8(node); case 5 -> FUN_80051B70(node,12,7); on 0 FUN_800517F8(node).
//   STATE 1 : node[3]<8 dispatches jt1 @0x80109DCC (the per-node[3] animation/spawn sub-states); the
//       cases drive node[5] sub-state, call FUN_80077B38 (model-attach, a1=0x8014C808) on entry, then
//       FUN_8004BD64 (5-arg, arg5 = &node[0x60] passed on the stack) and copy node[1] from node[0x10][1];
//       several gate on area bytes 0x800BF9DD/0x800BF9B5 to advance node[4]=3 / run the FUN_8004D4C4+
//       FUN_8004B0D8 tail. jt1 case 1 -> FUN_8018C574; case 3/4/5 -> FUN_8007778C.
//   STATE 2 : if node[3]==2 run the FUN_8004D4C4(29,1)+FUN_8004B0D8 tail, node[4]=3.
//   STATE 3 : FUN_8007A624(node).   STATE >=4 : nothing.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the direct node WRITES owned native;
// every sub-behavior CALL stays reachable via rec_dispatch (pure-PSX leaf). Guest a0..a3 + the ONE
// stack argument (sp+16) are set exactly as the guest does — so we mirror the guest prologue (sp -= 40)
// so FUN_8004BD64 reads arg5 from the same frame slot the recomp does (that slot lives in the gate's
// excluded stack window, so it never shows in the RAM diff, but the leaf must read the right value).
// Both jump tables READ live from resident overlay RAM. Transcribed 1:1 as a register machine; the
// byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net. NO GTE/render.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8012DA04u;
constexpr uint32_t A1_MODEL = 0x8014C808u;   // FUN_80077B38 arg (lui 0x8015 + addiu -14328)

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline void leaf2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static inline void leaf3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}
// 5-arg: a0..a3 in regs, arg5 on the guest stack at sp+16 (sp = our mirrored frame = entry-40).
static inline void leaf5(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t arg5, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
  c->mem_w32(c->r[29] + 16, arg5);
  rec_dispatch(c, fn);
}

void beh_typed_anim_spawn(Core* c) {
  uint32_t s0 = c->r[4];                            // s0 = a0 (node)
  uint32_t entry_sp = c->r[29];
  c->r[29] = entry_sp - 40;                         // mirror the guest prologue frame

  uint32_t v1 = c->mem_r8(s0 + 4);                  // node[4] = outer state
  uint8_t n3;
  uint32_t s1;

  if (v1 == 1) goto S1;
  if ((int32_t)v1 < 2) { if (v1 == 0) goto S0; goto Lret; }
  if (v1 == 2) goto S2;
  if (v1 == 3) goto S3;
  goto Lret;                                        // v1 >= 4

 // ---------------- STATE 0 ----------------
 S0:
  n3 = c->mem_r8(s0 + 3);
  if (!(n3 < 8)) goto Lret;
  c->mem_w8(s0 + 4, 1);                             // node[4] = 1 (delay-slot store)
  switch (n3) {
    case 0: case 1: case 2: case 6: case 7:         // jt0 -> 0x8012da90
      c->mem_w8 (s0 + 11, 16);                      // node[11] = 16
      c->mem_w8 (s0 + 13, 0);                       // node[13] = 0
      c->mem_w16(s0 + 0x5C, 0);
      c->mem_w16(s0 + 0x5A, 0);
      c->mem_w8 (s0 + 0x47, 0);
      c->mem_w16(s0 + 0x60, 0);
      c->mem_w8 (s0 + 8, 240);                      // node[8] = 240 (delay-slot store)
      goto Lret;
    case 3: case 4: {                               // jt0 -> 0x8012dab8
      leaf3(c, s0, 12, 79, 0x80051B70u);            // FUN_80051B70(node, 12, 79)
      if (c->r[2] != 0) goto Lret;
      uint32_t rec = c->mem_r32(s0 + 0xC0);         // node[0xC0]
      c->mem_w16(rec + 8, (uint16_t)(n3 == 3 ? 182 : (uint16_t)(int16_t)-182));
      leaf1(c, s0, 0x800517F8u);                    // FUN_800517F8(node)
      goto Lret;
    }
    case 5:                                         // jt0 -> 0x8012db08
      leaf3(c, s0, 12, 7, 0x80051B70u);             // FUN_80051B70(node, 12, 7)
      if (c->r[2] != 0) goto Lret;
      leaf1(c, s0, 0x800517F8u);                    // FUN_800517F8(node)
      goto Lret;
  }
  goto Lret;

 // ---------------- STATE 1 ----------------
 S1:
  n3 = c->mem_r8(s0 + 3);
  if (!(n3 < 8)) goto Lret;
  switch (n3) {
    case 0: {                                       // jt1 -> 0x8012db60
      s1 = c->mem_r8(s0 + 5);                       // node[5]
      if (s1 == 0) {                                // 0x8012db88
        if (c->mem_r8(0x800BF9DDu) != 9) goto Lret;
        c->mem_w8(s0 + 5, 1);                       // node[5] = 1
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u)); // node[0x3C] = *(0x800ECF80) (delay slot)
        leaf3(c, s0, A1_MODEL, 15, 0x80077B38u);    // FUN_80077B38(node, 0x8014C808, 15)
        c->mem_w16(s0 + 0x60, 64);
        c->mem_w16(s0 + 0x62, 0);
        c->mem_w16(s0 + 0x64, (uint16_t)(int16_t)-32);
        goto Lret;
      }
      if (s1 == 1) {                                // 0x8012dbd8
        leaf5(c, s0, 2, c->mem_r32(0x800E7F5Cu), c->mem_r32(0x800E7F50u),
              s0 + 0x60, 0x8004BD64u);              // FUN_8004BD64(node,2,*0x800E7F5C,*0x800E7F50,&node[0x60])
        c->mem_w8(s0 + 1, (uint8_t)s1);             // node[1] = s1 (=1)
        if (c->mem_r8(0x800BF9DDu) != 11) goto Lret;
        c->mem_w8(s0 + 4, 3);                       // node[4] = 3
        goto Lret;
      }
      goto Lret;                                    // s1 >= 2
    }
    case 1:                                         // jt1 -> 0x8012dc10
      leaf1(c, s0, 0x8018C574u);                    // FUN_8018C574(node)
      goto Lret;
    case 2: {                                       // jt1 -> 0x8012dc20
      s1 = c->mem_r8(s0 + 5);                       // node[5]
      uint32_t rec = c->mem_r32(s0 + 0x10);         // node[0x10]
      if (s1 == 0) {                                // 0x8012dc40
        c->mem_w8(s0 + 5, 1);                       // node[5] = 1
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u));
        leaf3(c, s0, A1_MODEL, 2, 0x80077B38u);     // FUN_80077B38(node, 0x8014C808, 2)
        c->mem_w16(s0 + 0x60, 32);
        c->mem_w16(s0 + 0x62, 16);
        c->mem_w16(s0 + 0x64, 0);
      } else if (s1 != 1) goto Lret;                // s1 >= 2
      // fall-through (s1==0 and s1==1) -> 0x8012dc7c
      leaf5(c, s0, 1, c->mem_r32(rec + 0xDC), 0, s0 + 0x60, 0x8004BD64u);
      c->mem_w8(s0 + 1, c->mem_r8(rec + 1));        // node[1] = node[0x10][1]
      goto Lret;
    }
    case 3: case 4: case 5:                         // jt1 -> 0x8012dca0
      leaf1(c, s0, 0x8007778Cu);                    // FUN_8007778C(node)
      goto Lret;
    case 6: {                                       // jt1 -> 0x8012dcb0
      s1 = c->mem_r8(s0 + 5);
      uint32_t rec = c->mem_r32(s0 + 0x10);
      if (s1 == 0) {                                // 0x8012dcd0
        c->mem_w8(s0 + 5, 1);
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u));
        leaf3(c, s0, A1_MODEL, 17, 0x80077B38u);    // FUN_80077B38(node, 0x8014C808, 17)
        c->mem_w16(s0 + 0x60, (uint16_t)(int16_t)-200);
        c->mem_w16(s0 + 0x62, 32);
        c->mem_w16(s0 + 0x64, 0);
      } else if (s1 != 1) goto Lret;
      // fall-through -> 0x8012dd0c
      leaf5(c, s0, 2, 0, c->mem_r32(rec + 0xD0), s0 + 0x60, 0x8004BD64u);
      c->mem_w8(s0 + 1, c->mem_r8(rec + 1));        // node[1] = node[0x10][1]
      if (c->mem_r8(0x800BF9B5u) != 5) goto Lret;
      goto Ltail14;                                 // 0x8012ddfc with a0=14
    }
    case 7: {                                       // jt1 -> 0x8012dd4c
      s1 = c->mem_r8(s0 + 5);
      uint32_t rec = c->mem_r32(s0 + 0x10);
      if (s1 == 0) {                                // 0x8012dd6c
        c->mem_w8(s0 + 5, 1);
        c->mem_w32(s0 + 0x3C, c->mem_r32(0x800ECF80u));
        leaf3(c, s0, A1_MODEL, 14, 0x80077B38u);    // FUN_80077B38(node, 0x8014C808, 14)
        c->mem_w16(s0 + 0x60, (uint16_t)(int16_t)-200);
        c->mem_w16(s0 + 0x62, 48);
        c->mem_w16(s0 + 0x64, 0);
      } else if (s1 != 1) goto Lret;
      // fall-through -> 0x8012dda8
      leaf5(c, s0, 2, 0, c->mem_r32(rec + 0xD0), s0 + 0x60, 0x8004BD64u);
      c->mem_w8(s0 + 1, c->mem_r8(rec + 1));        // node[1] = node[0x10][1]
      if (c->mem_r8(0x800BF9B5u) != 6) goto Lret;
      goto Ltail35;                                 // 0x8012ddfc with a0=35
    }
  }
  goto Lret;

 // ---------------- STATE 2 ----------------
 S2:
  if (c->mem_r8(s0 + 3) != 2) goto Lret;            // node[3] != node[4](=2)
  // a0 = 29 -> shared tail 0x8012ddfc
  leaf2(c, 29, 1, 0x8004D4C4u);                     // FUN_8004D4C4(29, 1)
  goto Ltail_after;

 Ltail14:
  leaf2(c, 14, 1, 0x8004D4C4u);                     // FUN_8004D4C4(14, 1)
  goto Ltail_after;
 Ltail35:
  leaf2(c, 35, 1, 0x8004D4C4u);                     // FUN_8004D4C4(35, 1)
  goto Ltail_after;
 Ltail_after:
  leaf1(c, s0, 0x8004B0D8u);                        // FUN_8004B0D8(node)
  c->mem_w8(s0 + 4, 3);                             // node[4] = 3
  goto Lret;

 // ---------------- STATE 3 ----------------
 S3:
  leaf1(c, s0, 0x8007A624u);                        // FUN_8007A624(node)
 Lret:
  c->r[29] = entry_sp;                              // restore caller sp (mirror epilogue)
  return;
}

void ov_beh_typed_anim_spawn(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("typed_anim_spawnverify") ? 1 : 0;
  if (!s_v) { beh_typed_anim_spawn(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_typed_anim_spawn(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[typed_anim_spawnverify] MISMATCH obj=%08x st=%u n3=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 3), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[typed_anim_spawnverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_typed_anim_spawn_run(Core* c) { ov_beh_typed_anim_spawn(c); }
