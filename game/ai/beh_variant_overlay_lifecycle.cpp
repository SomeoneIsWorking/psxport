// engine/beh_variant_overlay_lifecycle.cpp — PC-native per-object BEHAVIOR handler FUN_8007DC38.
//
// A RESIDENT MAIN.EXE per-object behavior (installed by FUN_8007E038, which sets node[0x1C]=this handler,
// node[0x47]=1, node[3]=variant, node[0x28]|=0x80, and seeds the record-table pointers node[0x48]/[0x4C]/
// [0x50] from the global list DAT_800ECF60, node[0x5C]=0xFFFF, node[0x5E]=record-index). A 4-phase
// lifecycle overlay actor (sibling of FUN_8007DDE0): it manages the global overlay-flag byte at
// 0x800BF822 (this instance owns bit 0x04), seeds its world position from a record table indexed by
// node[0x5E], and picks a variant anim/sprite id into node[0x18] from the variant byte node[3].
//
// Outer state machine on node[4] (uint8):
//   STATE 0 (spawn/init): pos = mem32(node+0x50) + (uint16)mem16( mem32(node+0x4C) + s16(node+0x5E)*4 );
//     write that to node[0x10] AND node[0x14]; FUN_8007C0D0(node,0); node[0x46]=1; node[4]++ (->1).
//     Then by variant byte node[3]: ==2 -> node[0x18]=5; ==0 -> node[0x18]=4 and OR bit 0x04 into
//     0x800BF822; ==3 -> node[0x18]=1; (==1 and >=4 do nothing).
//   STATE 1 (active): if node[3]==0 && (0x800BF822 & 0xFB)!=0 -> node[4]=2 (begin teardown). Always
//     FUN_8007C940(node); FUN_8007CC00(node); then if node[3]!=1 -> FUN_8005019C(node+0x54, node[0x18],
//     1, 2) (the per-frame anim/render step).
//   STATE 2: node[4]=3 (one-frame transition into despawn).
//   STATE 3 (despawn): if node[3]==0 -> clear bit 0x04 of 0x800BF822; FUN_8007A624(node) (despawn).
//
// CONTROL FLOW + every node-field / global-byte WRITE owned native at the exact offset/width; every
// sub-behavior CALL stays a pure-PSX leaf via rec_dispatch (set c->r[4..] then dispatch). node[3] is read
// as a byte (matching Ghidra: signed `char` only ever ==/!= tested in state 1, unsigned `byte` in the
// state-0 variant switch — equality/<3 are signedness-invariant here). node[0x5E] is a signed `short`,
// node[0x4C]/[0x50]/[0x10]/[0x14] are 32-bit, the table value is a `ushort`. Byte-exact A/B gate (full
// RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8007DC38u;
constexpr uint32_t OVL_FLAG = 0x800BF822u;   // DAT_800bf822 — global overlay-flag byte; this owns bit 0x04

static inline void leaf1(Core* c, uint32_t a0, uint32_t fn) {
  c->r[4] = a0; rec_dispatch(c, fn);
}
static inline void leaf2(Core* c, uint32_t a0, uint32_t a1, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static inline void leaf4(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t fn) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3; rec_dispatch(c, fn);
}

void beh_variant_overlay_lifecycle(Core* c) {
  const uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    // ---------- STATE 1 (active) ----------
    if (c->mem_r8(nd + 3) == 0 && (c->mem_r8(OVL_FLAG) & 0xfb) != 0) {
      c->mem_w8(nd + 4, 2);
    }
    leaf1(c, nd, 0x8007c940u);                              // FUN_8007C940
    leaf1(c, nd, 0x8007cc00u);                              // FUN_8007CC00
    if (c->mem_r8(nd + 3) != 1) {
      leaf4(c, nd + 0x54, c->mem_r8(nd + 0x18), 1, 2, 0x8005019cu);  // FUN_8005019C(node+0x54,node[0x18],1,2)
    }
  } else if (st < 2) {
    // ---------- STATE 0 (spawn/init) ----------  (st == 0)
    int32_t  base = (int32_t)c->mem_r32(nd + 0x4c);
    int16_t  idx  = (int16_t)c->mem_r16(nd + 0x5e);
    uint16_t tv   = c->mem_r16(base + (uint32_t)(int32_t)(idx * 4));
    int32_t  pos  = (int32_t)c->mem_r32(nd + 0x50) + (int32_t)(uint32_t)tv;
    c->mem_w32(nd + 0x10, (uint32_t)pos);
    c->mem_w32(nd + 0x14, (uint32_t)pos);
    leaf2(c, nd, 0, 0x8007c0d0u);                           // FUN_8007C0D0(node,0)
    uint8_t n3 = c->mem_r8(nd + 3);
    c->mem_w8(nd + 0x46, 1);
    c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));     // node[4] += 1  (-> 1)
    if (n3 == 2) {
      c->mem_w8(nd + 0x18, 5);
    } else if (n3 < 3) {
      if (n3 == 0) {
        c->mem_w8(nd + 0x18, 4);
        c->mem_w8(OVL_FLAG, (uint8_t)(c->mem_r8(OVL_FLAG) | 4));
      }
    } else if (n3 == 3) {
      c->mem_w8(nd + 0x18, 1);
    }
  } else if (st == 2) {
    // ---------- STATE 2 (transition) ----------
    c->mem_w8(nd + 4, 3);
  } else if (st == 3) {
    // ---------- STATE 3 (despawn) ----------
    if (c->mem_r8(nd + 3) == 0) {
      c->mem_w8(OVL_FLAG, (uint8_t)(c->mem_r8(OVL_FLAG) & 0xfb));
    }
    leaf1(c, nd, 0x8007a624u);                              // FUN_8007A624 (despawn)
  }
}

void ov_beh_variant_overlay_lifecycle(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("variant_overlay_lifecycleverify") ? 1 : 0;
  if (!s_v) { beh_variant_overlay_lifecycle(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_variant_overlay_lifecycle(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[variant_overlay_lifecycleverify] MISMATCH obj=%08x st=%u n3=%u ram@%x (nat=%02x ora=%02x) spad@%x\n",
                           obj, c->mem_r8(obj + 4), c->mem_r8(obj + 3), ro, ro >= 0 ? ramN[ro] : 0, ro >= 0 ? c->ram[ro] : 0, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[variant_overlay_lifecycleverify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_variant_overlay_lifecycle_run(Core* c) { ov_beh_variant_overlay_lifecycle(c); }
