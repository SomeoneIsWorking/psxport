// engine/beh_variant_actor_sm.cpp — PC-native per-object BEHAVIOR handler FUN_8011D578.
//
// Overlay handler (~x778/field-frame on seaside). Ported from the Ghidra decompile of FUN_8011D578
// (scratch/decomp/field2/8011d578.c), cross-checked against the raw disasm for the byte/half widths and
// the init gate. Outer state machine on node[4]:
//   STATE 0 : INIT, gated on FUN_800519E0(node, 0xD, mem32[0x800ECFB8], 0x8014C0BC) — bail if !=0. Else
//             node[0x7C]=0x8014DE54 (behavior table), node[0x3C]=mem32[0x800ECFBC], node[0x0B]=0x40,
//             node[0]=9, node[0x80]=0x50, node[0x82]=node[0x84]=0xA0, node[0x86]=0x120,
//             node[0x2C]=0x26DE0000, node[0x30]=0xFC040000, node[0x34]=0x16440000, node[0x2B]=node[0x29]=0,
//             node[0x58]=node[0x56]=node[0x54]=0; then per node[3]: ==0 -> node[0x56]=0xC00,node[0x7B]=7;
//             ==1 -> node[0x56]=0x400,node[0x7B]=0. FUN_80041718(node,node[0x7B],0); node[4]++.
//   STATE 1 : if node[3]==0 -> FUN_8011D108(node) then tail. If node[3]!=1 -> tail. Else (node[3]==1):
//             node[5]==0 & (scratch[0x207]!=10 || (i16)scratch[0x160]>=8000): v=FUN_8007778C(node);
//               v==0 -> if mem[0x800BF8BC]==255 node[4]=3; v!=0 & node[0x2B]==3 -> node[0x7A]=0x14,
//               FUN_80042354(1,1), FUN_80040D68(node,0x80148D2C), node[0x70]=2, node[5]++ (->LAB_d7d4).
//             node[5]==1: node[1]=1, FUN_80077E7C(node); if node[0x70]==255 node[5]-- (LAB_d7d4).
//             Then FUN_80041098(node), FUN_8004190C(node).
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).
//   TAIL (LAB_d7e8) : if node[1]!=0 -> FUN_800518FC(node) + FUN_8011D82C(node); node[0x2B]=0.
//
// CONTROL FLOW + the direct node WRITES owned native; every sub-behavior CALL stays reachable via
// rec_dispatch (pure-PSX leaf). NOTE: Ghidra typed several `lbu`-compare-to-0xFF as `== -1`; the real
// compares are `== 255`. scratch[0x160] is a SIGNED 16-bit (lh). The 0x2C/0x30/0x34 stores are 32-bit
// lui-only constants (low 16 = 0). The byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the
// safety net.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"   // world_despawn (FUN_8007A624)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8011D578u;

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
static inline uint32_t leafr4(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3; rec_dispatch(c, fn); return c->r[2];
}

void beh_variant_actor_sm(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st >= 2) {
      if (st == 2) return;
      if (st != 3) return;
      world_despawn(c, nd);                          // FUN_8007A624
      return;
    }
    if (st != 0) return;
    // STATE 0 (INIT)
    if (leafr4(c, nd, 0xd, c->mem_r32(0x800ecfb8u), 0x8014c0bcu, 0x800519e0u) != 0) return;  // FUN_800519E0
    c->mem_w32(nd + 0x7c, 0x8014de54u);
    c->mem_w32(nd + 0x3c, c->mem_r32(0x800ecfbcu));
    c->mem_w8(nd + 0x0b, 0x40);
    c->mem_w8(nd + 0, 9);
    c->mem_w16(nd + 0x80, 0x50);
    c->mem_w16(nd + 0x82, 0xa0);
    c->mem_w16(nd + 0x84, 0xa0);
    c->mem_w16(nd + 0x86, 0x120);
    c->mem_w32(nd + 0x2c, 0x26de0000u);
    c->mem_w32(nd + 0x30, 0xfc040000u);
    c->mem_w8(nd + 0x2b, 0);
    c->mem_w8(nd + 0x29, 0);
    c->mem_w16(nd + 0x58, 0);
    c->mem_w16(nd + 0x56, 0);
    c->mem_w16(nd + 0x54, 0);
    c->mem_w32(nd + 0x34, 0x16440000u);
    {
      uint8_t n3 = c->mem_r8(nd + 3);
      if (n3 == 0) { c->mem_w16(nd + 0x56, 0xc00); c->mem_w8(nd + 0x7b, 7); }
      else if (n3 == 1) { c->mem_w16(nd + 0x56, 0x400); c->mem_w8(nd + 0x7b, 0); }
    }
    leaf3(c, nd, c->mem_r8(nd + 0x7b), 0, 0x80041718u);   // FUN_80041718(node, node[0x7B], 0)
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
    return;
  }

  // STATE 1
  {
    uint8_t n3 = c->mem_r8(nd + 3);
    if (n3 == 0) { leaf1(c, nd, 0x8011d108u); goto Ltail; }   // FUN_8011D108
    if (n3 != 1) goto Ltail;

    uint8_t n5 = c->mem_r8(nd + 5);
    if (n5 == 0) {
      if (c->mem_r8(0x1f800207u) != 10 || (int16_t)c->mem_r16(0x1f800160u) >= 8000) {
        if (leafr1(c, nd, 0x8007778cu) == 0) {              // FUN_8007778C
          if (c->mem_r8(0x800bf8bcu) == 255) c->mem_w8(nd + 4, 3);
        } else if (c->mem_r8(nd + 0x2b) == 3) {
          c->mem_w8(nd + 0x7a, 0x14);
          leaf2(c, 1, 1, 0x80042354u);                      // FUN_80042354(1,1)
          leaf2(c, nd, 0x80148d2cu, 0x80040d68u);           // FUN_80040D68(node, 0x80148D2C)
          c->mem_w8(nd + 0x70, 2);
          c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) + 1));   // LAB_d7d4
        }
      }
    } else if (n5 == 1) {
      c->mem_w8(nd + 1, 1);
      leaf1(c, nd, 0x80077e7cu);                            // FUN_80077E7C
      if (c->mem_r8(nd + 0x70) == 255)
        c->mem_w8(nd + 5, (uint8_t)(c->mem_r8(nd + 5) - 1)); // LAB_d7d4
    }
    leaf1(c, nd, 0x80041098u);                              // FUN_80041098
    leaf1(c, nd, 0x8004190cu);                              // FUN_8004190C
  }

 Ltail:
  if (c->mem_r8(nd + 1) != 0) {
    leaf1(c, nd, 0x800518fcu);                              // FUN_800518FC
    leaf1(c, nd, 0x8011d82cu);                              // FUN_8011D82C
  }
  c->mem_w8(nd + 0x2b, 0);
}

void ov_beh_variant_actor_sm(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("variant_actor_smverify") ? 1 : 0;
  if (!s_v) { beh_variant_actor_sm(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_variant_actor_sm(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[variant_actor_smverify] MISMATCH obj=%08x st=%u n3=%u n5=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 3), c->mem_r8(obj + 5), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[variant_actor_smverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_variant_actor_sm_run(Core* c) { ov_beh_variant_actor_sm(c); }
