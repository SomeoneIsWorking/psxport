// game/ai/beh_camera_target_follow.cpp — PC-native per-object BEHAVIOR handler FUN_80059ED8.
//
// A RESIDENT MAIN.EXE per-object behavior (range 0x80059ED8..0x8005A398; `jr ra` at 0x8005a394). THE
// hottest still-PSX RESIDENT handler on seaside (~x741/field-frame). A camera/view tracker: node[0x10]
// points at the tracked target object (pcVar7); the handler derives the view extents (node[0x40]/[0x42]),
// the look-at target (node[0x48]/[0x4a]/[0x4c] + smooth params node[0x56]/[0x58]), and the scroll-clamp
// factors (node[0x4e]/[0x50]), then dispatches an area-specific camera routine by area byte 0x800BF870.
//
//   STATE 0 (init @0x80059f2c): if target[0x181]==0 clear scratchpad mem16(0x1f80019e); node[0x54]=0;
//     node[4]->1; pick view extents from sign of target[0x17e]; clear node[0xe]/[0x2b]/[0x46]/[0x5f].
//   STATE 1 (track @0x80059f90): bail if target[0]==0; node[1]=target[1], bail if 0. Optional camera-shake
//     (FUN_800312d4) gated on global 0x800BFE55 + |target[0x44]|>=0xC01 + (mem16(0x1f80017c)&7)==0. Copy
//     target xform (node[0x2c]/[0x30]/[0x34]/[0x2a]); compute the look-at via one of three branches
//     (target[0x29]==0||target[0x78]!=0 -> A: 0x16b==8 immediate, else FUN_800489e4 + scratchpad
//     0x1f8001a0..a6 gating; else -> B: target[0x6a] nibble!=2 copy). Then scroll-clamp node[0x4e]/[0x50]
//     to [0,0x80]/[0,0x100] (clearing node[1] on underflow). Finally, gated on 0x800BF873==0 &&
//     mem8(0x800bf80d)==0 && target[0x158]==0, switch(area 0x800BF870): 0/4/6 -> FUN_8010c5a8/80115afc/
//     80114294 (each gated on mem8(0x800bf816)==0), 8 -> FUN_8011332c (gated target[0x16b]==0),
//     0xb -> FUN_8010bc10, 0xe -> FUN_8010b238.
//   STATE 2 & 3 (@0x8005a380): both -> FUN_8007a624(node).
//
// CONTROL FLOW + every node/global/scratchpad WRITE owned native byte-for-byte; every sub-behavior CALL
// stays a pure-PSX leaf via rec_dispatch. Goto labels = guest addresses so delay-slot store ordering is
// exact. RE'd 1:1 from disas 0x80059ED8 (Ghidra decomp scratch/decomp/field2/80059ed8.c cross-checked).
// GOTCHAs: target[0x17e]/[0x44]/[0x32]/[0x4a] are signed `lh`; mem16(0x1f80017c) is `lhu`; the 0x1f8001a6
// nibble = ((int16)v>>8)&0xf and sign bit = (int16)v & 0x8000; scroll math uses arithmetic >>2 on a signed
// delta. Byte-exact A/B gate (full RAM+scratchpad vs rec_super_call) is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80059ED8u;

static inline int16_t  s16(Core* c, uint32_t a) { return c->mem_r16s(a); }

}  // namespace

void beh_camera_target_follow(Core* c) {
  const uint32_t nd = c->r[4];                          // s0 = node
  const uint32_t p  = c->mem_r32(nd + 0x10);            // s1 = target (pcVar7), read unconditionally
  uint8_t st = c->mem_r8(nd + 4);

  if (st != 1) {
    if (st >= 2) {
      if (st >= 4) return;                              // 0x80059f18: st>=4 -> return
      eng(c).spawn.despawn(nd);       // 0x8005a380: states 2 & 3
      return;
    }
    if (st != 0) return;
    // ---------- STATE 0 (init @0x80059f2c) ----------
    if (c->mem_r8(p + 0x181) == 0) c->mem_w16(0x1F80019Eu, 0);  // 0x80059f3c (only if target[0x181]==0)
    c->mem_w16(nd + 0x54, 0);
    c->mem_w8(nd + 4, (uint8_t)(st + 1));               // node[4] -> 1
    if (s16(c, p + 0x17e) < 0) { c->mem_w16(nd + 0x40, 0x28); c->mem_w16(nd + 0x42, 0x37); }
    else                       { c->mem_w16(nd + 0x40, 0x50); c->mem_w16(nd + 0x42, 0x6e); }
    c->mem_w16(nd + 0x0e, 0);
    c->mem_w8(nd + 0x2b, 0);
    c->mem_w8(nd + 0x46, 0);
    c->mem_w8(nd + 0x5f, 0);
    return;
  }

  // ================= STATE 1 (track @0x80059f90) =================
  if (c->mem_r8(p + 0) == 0) return;                    // 0x80059f90
  uint8_t b1 = c->mem_r8(p + 1);
  c->mem_w8(nd + 1, b1);                                // node[1] = target[1]
  if (b1 == 0) return;                                  // 0x80059fb0

  if (c->mem_r8(0x800BFE55u) != 0) {                    // camera-shake gate
    int iv = (int)s16(c, p + 0x44);
    if (iv < 0) iv = -iv;                               // abs
    if (iv >= 0xc01 && (c->mem_r16(0x1F80017Cu) & 7) == 0) {  // |..|>=3073 && (mem16(0x1f80017c)&7)==0
      c->r[4] = 0x2c; c->r[5] = p + 0x2c; c->r[6] = 0xFFFFFFECu;  // FUN_800312d4(0x2c, target+0x2c, -20)
      rec_dispatch(c, 0x800312D4u);
    }
  }

  if (s16(c, p + 0x17e) < 0) { c->mem_w16(nd + 0x40, 0x28); c->mem_w16(nd + 0x42, 0x37); }
  else                       { c->mem_w16(nd + 0x40, 0x50); c->mem_w16(nd + 0x42, 0x6e); }
  c->mem_w32(nd + 0x2c, c->mem_r32(p + 0x2c));
  c->mem_w32(nd + 0x30, c->mem_r32(p + 0x30));
  c->mem_w32(nd + 0x34, c->mem_r32(p + 0x34));
  c->mem_w8 (nd + 0x2a, c->mem_r8 (p + 0x2a));

  bool goto_L1ac = false, set_node1_zero = false;
  if (c->mem_r8(p + 0x29) == 0 || c->mem_r8(p + 0x78) != 0) {
    // ---- A-block @0x8005a0e0 ----
    if (c->mem_r8(p + 0x16b) == 8) {                    // immediate look-at
      c->mem_w16(nd + 0x48, c->mem_r16(nd + 0x2e));
      c->mem_w16(nd + 0x4c, c->mem_r16(nd + 0x36));
      c->mem_w16(nd + 0x58, 0);
      c->mem_w16(nd + 0x56, c->mem_r16(0x1F800210u));
      c->mem_w16(nd + 0x4a, c->mem_r16(0x800BF812u));
      goto_L1ac = true;
    } else {
      c->r[4] = nd; c->r[5] = (uint32_t)(int32_t)s16(c, nd + 0x32);
      rec_dispatch(c, 0x800489E4u);                     // FUN_800489e4(node, (s16)node[0x32])
      uint32_t iv5 = c->r[2];
      uint16_t da6 = c->mem_r16(0x1F8001A6u);
      if (iv5 != 0) {
        uint32_t nib = ((uint32_t)(int16_t)da6 >> 8) & 0xf;          // ((int16)v>>8)&0xf
        bool signbit = ((int16_t)da6 & 0x8000) != 0;                 // sign bit of (int16)v
        if (nib != 2 && nib != 7 && !signbit && (int)s16(c, nd + 0x32) <= (int)s16(c, 0x1F8001A4u)) {
          c->mem_w16(nd + 0x4a, c->mem_r16(0x1F8001A4u));
          c->mem_w16(nd + 0x48, c->mem_r16(nd + 0x2e));
          c->mem_w16(nd + 0x4c, c->mem_r16(nd + 0x36));
          c->mem_w16(nd + 0x56, c->mem_r16(0x1F8001A0u));            // uVar4
          c->mem_w16(nd + 0x58, c->mem_r16(0x1F8001A2u));            // uVar3
          goto_L1ac = true;
        } else set_node1_zero = true;
      } else set_node1_zero = true;
    }
  } else if (((c->mem_r16(p + 0x6a) >> 8) & 0xf) != 2) {
    // ---- B-block @0x8005a09c ----
    c->mem_w16(nd + 0x48, c->mem_r16(p + 0x2e));
    c->mem_w16(nd + 0x4a, (uint16_t)(c->mem_r16(p + 0x32) + c->mem_r16(p + 0x84)));
    c->mem_w16(nd + 0x4c, c->mem_r16(p + 0x36));
    c->mem_w16(nd + 0x56, c->mem_r16(p + 0x140));
    c->mem_w16(nd + 0x58, c->mem_r16(p + 0x142));
    goto_L1ac = true;
  } else {
    set_node1_zero = true;
  }
  if (!goto_L1ac && set_node1_zero) c->mem_w8(nd + 1, 0);  // @0x8005a1a8

  // ---- L1ac @0x8005a1ac: scroll-clamp (gated on node[1]) ----
  if (c->mem_r8(nd + 1) != 0) {
    int delta = (int)s16(c, nd + 0x4a) - (int)s16(c, p + 0x32);
    int v = 0x80 - ((delta - 0x78) >> 2);
    c->mem_w16(nd + 0x4e, (uint16_t)v);
    int16_t v16 = (int16_t)v;
    if (v16 < 0)        { c->mem_w16(nd + 0x4e, 0); c->mem_w8(nd + 1, 0); }
    else if (v16 > 0x80) c->mem_w16(nd + 0x4e, 0x80);

    int delta2 = (int)s16(c, nd + 0x4a) - (int)s16(c, p + 0x32);
    int v2 = 0x100 - ((delta2 - 0x78) >> 2);
    c->mem_w16(nd + 0x50, (uint16_t)v2);
    int16_t v2_16 = (int16_t)v2;
    if (v2_16 < 0)         { c->mem_w16(nd + 0x50, 0); c->mem_w8(nd + 1, 0); }
    else if (v2_16 > 0x100) c->mem_w16(nd + 0x50, 0x100);
  }

  // ---- area-camera dispatch @0x8005a254 ----
  if (c->mem_r8(0x800BF873u) == 0 && c->mem_r8(0x800BF80Du) == 0 && c->mem_r32(p + 0x158) == 0) {
    uint8_t area = c->mem_r8(0x800BF870u);
    if (area >= 15) return;                             // sltiu 15 default
    switch (area) {
      case 0:    if (c->mem_r8(0x800BF816u) == 0) { c->r[4]=nd; c->r[5]=p; rec_dispatch(c, 0x8010C5A8u); } break;
      case 4:    if (c->mem_r8(0x800BF816u) == 0) { c->r[4]=nd; c->r[5]=p; rec_dispatch(c, 0x80115AFCu); } break;
      case 6:    if (c->mem_r8(0x800BF816u) == 0) { c->r[4]=nd; c->r[5]=p; rec_dispatch(c, 0x80114294u); } break;
      case 8:    if (c->mem_r8(p + 0x16b) == 0)   { c->r[4]=nd; c->r[5]=p; rec_dispatch(c, 0x8011332Cu); } break;
      case 0xb:                                     { c->r[4]=nd; c->r[5]=p; rec_dispatch(c, 0x8010BC10u); } break;
      case 0xe:                                     { c->r[4]=nd; c->r[5]=p; rec_dispatch(c, 0x8010B238u); } break;
      default: break;
    }
  }
}
