// engine/beh_lift_platform.cpp — PC-native per-object BEHAVIOR handler FUN_8013A330.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8013A330
// (scratch/decomp/field2/8013a330.c), cross-checked against the raw disasm. A multi-segment (13-record)
// vertical mover that FOLLOWS a parent object's direction. Registers itself as the singleton
// mem32[0x800BF854]. Outer state machine on node[4]:
//   STATE 0 : INIT — mem32[0x800BF854]=node, node[0xBF]=0; if (i16)mem[0x800ED098] < 13 -> node[4]=3.
//             Else node[8]=node[9]=13, node[0x0B]=node[0x0D]=0, allocate 13 child records (FUN_8007AAE8)
//             into node[0xC0+4*i] + FUN_80051B04(rec,0xC,i+0x26); node[3]=0; if mem[0x800BFAD8]==0 &&
//             mem[0x800BF8B9]!=255 -> FUN_80118974(node[0xD0]); FUN_8013A184(node); FUN_8013989C(node);
//             then node[0]=1, node[0x82]=0xC0, node[0x29]=0, node[0x80]=0x60, node[0x84]=0x10,
//             node[0x86]=0x60, node[4]++.
//   STATE 1 : if (mem[0x800BF89C]==2 || mem[0x800E7EAA]!=1): FUN_800778E4(node, sign16(scratch[0x162]-
//             node[0x32])), s0=node[0x10] (the parent); node[1]==0 -> if parent[1]!=0 set node[1]=1 +
//             FUN_80077EBC. Else (the else-branch) s0 = the INCOMING guest s0 (c->r[16]) — faithful to
//             the recomp's uninitialized-register flow. If node[1]==0 -> tail. Then a direction machine:
//             node[0x5E] mirrors parent[0x5E] (1<->2), drives node[0x30] up/down by node[0x50]*±0x100,
//             clamps to node[0x60]/node[0x62] (toggling mem[0x800BF9EE] + node[0xBF]) and plays SFX 0x8D
//             (FUN_80074590) when parent[0xC0][0xC]&0xF00==0; a node[5] sub-machine (FUN_80139E64/
//             FUN_80139C2C/FUN_8013A008) advances/retreats; FUN_800517F8(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//   TAIL : write node[1] into each record[0x3F] (node[8] records); node[0x29]=0; FUN_80139A70(node).
//
// CONTROL FLOW + the direct node/record/global WRITES owned native; every sub-behavior CALL stays
// reachable via rec_dispatch (pure-PSX leaf). GOTCHAs vs Ghidra: mem[0x800BF8B9] `== -1` is `== 255`
// (lbu); the `unaff_s0` else-path uses the incoming guest s0 = c->r[16] (callee-saved -> preserved across
// rec_dispatch); mem[0x800ED098] is a SIGNED lh. The byte-exact A/B gate is the safety net.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn (FUN_8007A624)
#include "graphics_bind.h"   // ov_obj_render_update (FUN_800517F8)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8013A330u;

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) { c->r[4] = a0; rec_dispatch(c, fn); }
static inline void leaf2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static inline void leaf3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}
static inline uint32_t leafr1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn); return c->r[2];
}
static inline uint32_t leafr2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn); return c->r[2];
}

// LAB_8013A60C: parent[0xC0][0xC] & 0xF00 == 0 -> SFX 0x8D.
static inline void lift_sfx(Core* c, uint32_t s0) {
  if ((c->mem_r16(c->mem_r32(s0 + 0xc0) + 0xc) & 0xf00) == 0)
    leaf3(c, 0x8d, 0, 0, 0x80074590u);                     // FUN_80074590(0x8D,0,0)
}

void beh_lift_platform(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st >= 2) {
      if (st == 2) return;
      if (st != 3) return;
      world_despawn(c, nd);                           // FUN_8007A624
      return;
    }
    if (st != 0) return;
    // STATE 0 (INIT)
    c->mem_w32(0x800bf854u, nd);
    c->mem_w8(nd + 0xbf, 0);
    if ((int16_t)c->mem_r16(0x800ed098u) < 13) { c->mem_w8(nd + 4, 3); return; }
    c->mem_w8(nd + 8, 13);
    c->mem_w8(nd + 9, 13);
    c->mem_w8(nd + 0x0b, 0);
    c->mem_w8(nd + 0x0d, 0);
    c->r[4] = nd;                                           // a0 for the first FUN_8007AAE8
    {
      uint32_t base = nd;
      int i = 0;
      do {
        c->engine.graphicsBind.recordAlloc();                      // FUN_8007AAE8 -> rec (a0 carried)
        uint32_t rec = c->r[2];
        c->mem_w32(base + 0xc0, rec);
        c->r[4] = rec; c->r[5] = 0xc; c->r[6] = (uint32_t)(i + 0x26); rec_dispatch(c, 0x80051b04u);
        i++;
        base += 4;
      } while (i < (int)c->mem_r8(nd + 8));
    }
    c->mem_w8(nd + 3, 0);
    if (c->mem_r8(0x800bfad8u) == 0 && c->mem_r8(0x800bf8b9u) != 255)
      leaf1(c, c->mem_r32(nd + 0xd0), 0x80118974u);        // FUN_80118974(node[0xD0])
    leaf1(c, nd, 0x8013a184u);                             // FUN_8013A184
    leaf1(c, nd, 0x8013989cu);                             // FUN_8013989C
    c->mem_w8(nd + 0, 1);
    c->mem_w16(nd + 0x82, 0xc0);
    c->mem_w8(nd + 0x29, 0);
    c->mem_w16(nd + 0x80, 0x60);
    c->mem_w16(nd + 0x84, 0x10);
    c->mem_w16(nd + 0x86, 0x60);
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
    return;
  }

  // STATE 1
  uint32_t s0 = c->r[16];                                  // incoming guest s0 (else-path default)
  if (c->mem_r8(0x800bf89cu) == 2 || c->mem_r8(0x800e7eaau) != 1) {
    int32_t a1 = (int16_t)(uint16_t)((uint16_t)c->mem_r16(0x1f800162u) - (uint16_t)c->mem_r16(nd + 0x32));
    leaf2(c, nd, (uint32_t)a1, 0x800778e4u);               // FUN_800778E4(node, sign16(...))
    s0 = c->mem_r32(nd + 0x10);
    if (c->mem_r8(nd + 1) == 0) {
      if (c->mem_r8(s0 + 1) != 0) { c->mem_w8(nd + 1, 1); leaf1(c, nd, 0x80077ebcu); }  // FUN_80077EBC
      if (c->mem_r8(nd + 1) == 0) goto L6c4;               // L508 re-check
    }
  } else {
    if (c->mem_r8(nd + 1) == 0) goto L6c4;                 // L508
  }

  // ---- node[0x5E] direction machine (uses s0) ----
  {
    uint8_t pv = c->mem_r8(s0 + 0x5e);                     // parent[0x5E]
    if (pv == 1) c->mem_w8(nd + 0x5e, 2);
    else if (pv == 2) c->mem_w8(nd + 0x5e, 1);
    else c->mem_w8(nd + 0x5e, 0);

    if (c->mem_r8(nd + 0x5e) == 1) {
      c->mem_w32(nd + 0x30, (uint32_t)((int32_t)c->mem_r32(nd + 0x30) + (int16_t)c->mem_r16(nd + 0x50) * -0x100));
      if ((int16_t)c->mem_r16(nd + 0x60) < (int16_t)c->mem_r16(nd + 0x32)) {
        c->mem_w8(0x800bf9eeu, (uint8_t)(c->mem_r8(0x800bf9eeu) & 0xfe));
        lift_sfx(c, s0);
      } else {
        c->mem_w32(nd + 0x30, (uint32_t)((int32_t)(int16_t)c->mem_r16(nd + 0x60) << 16));
        c->mem_w8(0x800bf9eeu, (uint8_t)(c->mem_r8(0x800bf9eeu) | 1));
        c->mem_w8(nd + 0x5e, 0);
        c->mem_w8(nd + 0xbf, 0);
      }
    } else if (c->mem_r8(nd + 0x5e) == 2) {
      c->mem_w32(nd + 0x30, (uint32_t)((int32_t)c->mem_r32(nd + 0x30) + (int16_t)c->mem_r16(nd + 0x50) * 0x100));
      if ((int16_t)c->mem_r16(nd + 0x32) < (int16_t)c->mem_r16(nd + 0x62)) {
        lift_sfx(c, s0);
      } else {
        c->mem_w32(nd + 0x30, (uint32_t)((int32_t)(int16_t)c->mem_r16(nd + 0x62) << 16));
        c->mem_w8(nd + 0x5e, 0);
        c->mem_w8(nd + 0xbf, 1);
      }
    }

    // ---- node[5] sub-machine ----
    if (c->mem_r8(nd + 5) == 0) {
      if (c->mem_r8(nd + 0x5e) == 1) {
        if (leafr1(c, nd, 0x80139e64u) != 0) {             // FUN_80139E64
          c->mem_w8(nd + 6, 0);
          c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));
        }
      } else if (c->mem_r8(nd + 0x5e) == 2) {
        leaf1(c, nd, 0x80139c2cu);                         // FUN_80139C2C
      }
    } else if (c->mem_r8(nd + 5) == 1 && leafr2(c, nd, s0, 0x8013a008u) != 0) {  // FUN_8013A008(node, s0)
      c->mem_w8(nd + 6, 0);
      c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) - 1));
    }
    c->r[4] = nd; c->engine.graphicsBind.renderUpdate();                             // FUN_800517F8
  }

 L6c4:
  {
    uint8_t n8 = c->mem_r8(nd + 8);
    uint8_t n1 = c->mem_r8(nd + 1);
    uint32_t base = nd;
    for (int i = 0; i < (int)n8; i++) { c->mem_w8(c->mem_r32(base + 0xc0) + 0x3f, n1); base += 4; }
    c->mem_w8(nd + 0x29, 0);
    leaf1(c, nd, 0x80139a70u);                             // FUN_80139A70
  }
}

void ov_beh_lift_platform(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("lift_platformverify") ? 1 : 0;
  if (!s_v) { beh_lift_platform(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_lift_platform(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[lift_platformverify] MISMATCH obj=%08x st=%u n5e=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 0x5e), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[lift_platformverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_lift_platform_run(Core* c) { ov_beh_lift_platform(c); }
