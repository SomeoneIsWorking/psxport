// game/ai/beh_a08_scene_actor.cpp — PC-native port of A08's scene-actor behavior + its cutscene
// director. Closes the last of the 3 un-owned FUN_8007E9C8 fade callers named in the sibling
// finding (docs/findings/render.md "Un-owned FUN_8007E9C8 fade callers").
//
// Guest layout:
//   0x801280D0  — outer per-object behavior handler (this file's entry). 21 fnptr refs in A08 →
//                 real per-object beh installed on many scene actors. Registered in
//                 BehaviorDispatch::kTable at 0x801280D0.
//   0x80127C58  — the 10-state cutscene director (state-1 case-0x1A of the outer). State 9 is
//                 the additive-gray fade-out that ends with `DAT_800BFA50=0x16` + node[+4]=3
//                 despawn. Ported inline as `cutsceneDirector` — reached via a PLAIN C CALL from
//                 the native state1Run's case 0x1A, so the fade fires natively via
//                 fade(c).applyLeafCall.
//
// RE'd from Ghidra 12.0.4 A08 project + hand-disas spot-verify (scratch/decomp/a08_cutscene_
// director.c). Substrate leaves kept reachable via rec_dispatch (each a small standalone leaf):
//   0x800778E4  = position update
//   0x8004BD64  = position sync from parent
//   0x80051B70  = music/anim queue+ready check (returns 0 when ready)
//   0x80051B04  = one-shot SFX cue (from FUN_80127C58 state 2/3 head)
//   0x80074590  = SFX trigger
//   0x800517F8  = per-frame flag-apply
//   0x80077b38  = obj model attach (state 0 case 5/0x16)
//   0x8004766c  0x80048750  = state-0 case-0xA init leaves
//   0x8003116c  = subobj spawn helper
//   0x8007A624  = despawn (state 3)
//   0x8007778C  = position anim step (case 0xB / caseD_10)
//   0x800778e4  = position-Z leaf
//   0x8004D4C4  0x8004B0D8  = state-1 case-5 branch
//   0x8004DAEC  = state-1 case-0x12 helper
//   0x80077EBC  = state-1 case-0x1B leaf
//   0x80081218  = state-1 case-0x1A tail sprite draw
//   0x80126AD8  0x80126C68  0x80126E24  0x801273B4  0x801277E0  = per-variant leaves (still substrate)
//   0x801268D8  = state-0 case-0xA subobj spawn
//   0x801279DC  0x80127B34  = state-0 case-0x19/0x1A leaves
//   0x8013DD48  = sub-obj dispatcher (called from cutsceneDirector state 4/5/6)

#include "core.h"
#include "game_ctx.h"
#include "core/engine.h"
#include "render/screen_fade.h"
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {

// Guest globals this behavior consults.
constexpr uint32_t G_1F8000E2 = 0x1F8000E2u;   // scratchpad hword (source for state-1 base pos)
constexpr uint32_t G_1F8001A0 = 0x1F8001A0u;
constexpr uint32_t G_1F80017C = 0x1F80017Cu;
constexpr uint32_t G_800BF8DF = 0x800BF8DFu;
constexpr uint32_t G_800BF8E2 = 0x800BF8E2u;
constexpr uint32_t G_800BF8E3 = 0x800BF8E3u;
constexpr uint32_t G_800BF816 = 0x800BF816u;
constexpr uint32_t G_800BF922 = 0x800BF922u;
constexpr uint32_t G_800BF928 = 0x800BF928u;
constexpr uint32_t G_800BF93E = 0x800BF93Eu;
constexpr uint32_t G_800BFA30 = 0x800BFA30u;
constexpr uint32_t G_800BFA31 = 0x800BFA31u;
constexpr uint32_t G_800BFA32 = 0x800BFA32u;
constexpr uint32_t G_800BFA3A = 0x800BFA3Au;
constexpr uint32_t G_800BFA3B = 0x800BFA3Bu;
constexpr uint32_t G_800BFA3F = 0x800BFA3Fu;
constexpr uint32_t G_800BFA42 = 0x800BFA42u;
constexpr uint32_t G_800BFA4E = 0x800BFA4Eu;
constexpr uint32_t G_800BFA50 = 0x800BFA50u;
constexpr uint32_t G_800BFB22 = 0x800BFB22u;
constexpr uint32_t G_800E7F50 = 0x800E7F50u;
constexpr uint32_t G_800E7F5C = 0x800E7F5Cu;
constexpr uint32_t G_800ECF80 = 0x800ECF80u;
constexpr uint32_t G_800ED098 = 0x800ED098u;

// Data pointers.
constexpr uint32_t PTR_801486EC = 0x801486ECu;   // model list (state-0 case 5/0x16)
constexpr uint32_t DAT_801459AC = 0x801459ACu;   // state-0 cases 0..3 arg table (s16 stride)
constexpr uint32_t DAT_801459BC = 0x801459BCu;   // state-0 case 0xA subobj list
constexpr uint32_t DAT_80145A64 = 0x80145A64u;   // state-1 case-0x1A sprite id table (u8 stride)
constexpr uint32_t FN_8011973C  = 0x8011973Cu;   // state-0 case-0x18 handler ptr
constexpr uint32_t FN_8013DC04  = 0x8013DC04u;   // spawner obj handler (from FUN_8013DD48)

// Object field offsets used.
constexpr uint32_t O_KIND    = 0x00u;
constexpr uint32_t O_FLAG_01 = 0x01u;
constexpr uint32_t O_VARIANT = 0x03u;   // 0..0x1B
constexpr uint32_t O_STATE_4 = 0x04u;   // outer state 0..3
constexpr uint32_t O_SUB_5   = 0x05u;   // per-case sub-state
constexpr uint32_t O_SUB_6   = 0x06u;   // cutscene sub-state
constexpr uint32_t O_H_2E    = 0x2Eu;
constexpr uint32_t O_H_32    = 0x32u;
constexpr uint32_t O_H_36    = 0x36u;
constexpr uint32_t O_H_40    = 0x40u;
constexpr uint32_t O_H_42    = 0x42u;
constexpr uint32_t O_H_54    = 0x54u;
constexpr uint32_t O_H_56    = 0x56u;
constexpr uint32_t O_H_58    = 0x58u;
constexpr uint32_t O_H_5A    = 0x5Au;
constexpr uint32_t O_H_5C    = 0x5Cu;
constexpr uint32_t O_H_60    = 0x60u;
constexpr uint32_t O_H_80    = 0x80u;
constexpr uint32_t O_H_82    = 0x82u;
constexpr uint32_t O_H_84    = 0x84u;
constexpr uint32_t O_H_86    = 0x86u;
constexpr uint32_t O_B_08    = 0x08u;
constexpr uint32_t O_B_0B    = 0x0Bu;
constexpr uint32_t O_B_0D    = 0x0Du;
constexpr uint32_t O_B_29    = 0x29u;
constexpr uint32_t O_B_47    = 0x47u;
constexpr uint32_t O_B_5F    = 0x5Fu;
constexpr uint32_t O_D_10    = 0x10u;
constexpr uint32_t O_D_18    = 0x18u;
constexpr uint32_t O_D_3C    = 0x3Cu;
constexpr uint32_t O_D_C0    = 0xC0u;
constexpr uint32_t O_D_C4    = 0xC4u;
constexpr uint32_t O_D_D0    = 0xD0u;
constexpr uint32_t O_D_DC    = 0xDCu;
constexpr uint32_t O_ANIM_2C = 0x2Cu;   // used as base ptr for FUN_8013DD48

inline void call1(Core* c, uint32_t a, uint32_t addr) {
  c->r[4] = a; rec_dispatch(c, addr);
}
inline uint32_t call1Ret(Core* c, uint32_t a, uint32_t addr) {
  c->r[4] = a; rec_dispatch(c, addr); return c->r[2];
}
inline void call2(Core* c, uint32_t a, uint32_t b, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; rec_dispatch(c, addr);
}
inline uint32_t call2Ret(Core* c, uint32_t a, uint32_t b, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; rec_dispatch(c, addr); return c->r[2];
}
inline void call3(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; c->r[6] = d; rec_dispatch(c, addr);
}
inline uint32_t call3Ret(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; c->r[6] = d; rec_dispatch(c, addr); return c->r[2];
}
inline void call4(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; c->r[6] = d; c->r[7] = e; rec_dispatch(c, addr);
}
inline void call5(Core* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t f, uint32_t addr) {
  c->r[4] = a; c->r[5] = b; c->r[6] = d; c->r[7] = e;
  const uint32_t sp = c->r[29] - 24u; c->r[29] = sp;
  c->mem_w32(sp + 16u, f);   // MIPS calling convention: 5th arg on stack at sp+16
  rec_dispatch(c, addr);
  c->r[29] = sp + 24u;
}

// FUN_8013DD48(objAnim, subId) — allocate a spawner obj and hook its handler.
uint32_t sub8013DD48(Core* c, uint32_t param1, uint32_t param2) {
  // FUN_80072DDC(0, 0, 2, 0x47) — obj allocator
  const uint32_t sp_save = c->r[29];
  c->r[4] = 0u; c->r[5] = 0u; c->r[6] = 2u; c->r[7] = 0x47u;
  rec_dispatch(c, 0x80072DDCu);
  const uint32_t iVar1 = c->r[2];
  c->r[29] = sp_save;
  if (iVar1 != 0) {
    c->mem_w32(iVar1 + 0x1Cu, FN_8013DC04);
    c->mem_w8 (iVar1 + 0x03u, (uint8_t)param2);
    c->mem_w8 (iVar1 + 0x28u, (uint8_t)(c->mem_r8(iVar1 + 0x28u) | 0x80u));
    c->mem_w16(iVar1 + 0x2Eu, c->mem_r16(param1 + 2u));
    c->mem_w16(iVar1 + 0x32u, c->mem_r16(param1 + 6u));
    c->mem_w16(iVar1 + 0x36u, c->mem_r16(param1 + 10u));
  }
  return iVar1;
}

// ── FUN_80127C58 — the 10-state cutscene director ───────────────────────────────────────────────
// Runs each frame while state 1 case 0x1A is active. State 9 is the additive-gray fade-out that
// closes bug #27 for A08. Verbatim from Ghidra decomp with hand-disas spot-verify.
void cutsceneDirector(Core* c, uint32_t obj) {
  const uint8_t st = c->mem_r8(obj + O_SUB_5);
  bool fellThrough = false;

  switch (st) {
    case 0:
      c->mem_w16(obj + O_H_40, 0x000Cu);
      c->mem_w16(obj + O_H_42, 0u);
      c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
      [[fallthrough]];
    case 1: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) - 1);
      c->mem_w16(obj + O_H_40, (uint16_t)v);
      if (v != -1) return;
      c->mem_w16(obj + O_H_40, 0x003Cu);
      c->mem_w8(obj + O_SUB_5, (uint8_t)(c->mem_r8(obj + O_SUB_5) + 1u));
      return;
    }
    case 2: {
      call5(c, obj, 0u, c->mem_r32(G_800E7F5C), c->mem_r32(G_800E7F50), 0u, 0x8004BD64u);
      const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) - 1);
      c->mem_w16(obj + O_H_40, (uint16_t)v);
      c->mem_w16(obj + O_H_36, (uint16_t)(c->mem_r16(obj + O_H_36) + 0x14u));
      if (v != -1) break;
      c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
      break;
    }
    case 3: {
      call5(c, obj, 0u, c->mem_r32(G_800E7F5C), c->mem_r32(G_800E7F50), 0u, 0x8004BD64u);
      c->mem_w16(obj + O_H_36, (uint16_t)(c->mem_r16(obj + O_H_36) + 0x14u));
      const uint32_t c4 = c->mem_r32(obj + O_D_C4);
      const int16_t nv = (int16_t)(c->mem_r16(c4 + 8u) - 0x40);
      c->mem_w16(c4 + 8u, (uint16_t)nv);
      if (nv < -0x700) {
        c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
        c->r[4] = obj + O_ANIM_2C; c->r[5] = 0u;
        rec_dispatch(c, 0x8013DD48u);
        c->mem_w16(obj + O_H_40, 8u);
      }
      break;
    }
    case 4: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) - 1);
      c->mem_w16(obj + O_H_40, (uint16_t)v);
      if (v != -1) break;
      c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
      c->r[4] = obj + O_ANIM_2C;
      rec_dispatch(c, 0x8013DD48u);
      c->r[4] = obj + O_ANIM_2C; c->r[5] = 2u;
      rec_dispatch(c, 0x8013DD48u);
      c->mem_w16(obj + O_H_40, 8u);
      break;
    }
    case 5: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) - 1);
      c->mem_w16(obj + O_H_40, (uint16_t)v);
      if (v != -1) break;
      const uint32_t iVar8 = obj + O_ANIM_2C;
      c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
      c->r[4] = iVar8; c->r[5] = 3u; rec_dispatch(c, 0x8013DD48u);
      c->r[4] = iVar8; c->r[5] = 4u; rec_dispatch(c, 0x8013DD48u);
      c->r[4] = iVar8; c->r[5] = 5u; rec_dispatch(c, 0x8013DD48u);
      c->r[4] = iVar8; c->r[5] = 6u; rec_dispatch(c, 0x8013DD48u);
      c->mem_w16(obj + O_H_40, 8u);
      break;
    }
    case 6: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) - 1);
      c->mem_w16(obj + O_H_40, (uint16_t)v);
      if (v == -1) {
        c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
        for (int iVar8 = 7; iVar8 < 0x14; iVar8++) {
          c->r[4] = obj + O_ANIM_2C; c->r[5] = (uint32_t)iVar8;
          rec_dispatch(c, 0x8013DD48u);
        }
        c->mem_w16(obj + O_H_40, 0x5Au);
      }
      break;
    }
    case 7: {
      const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) - 1);
      c->mem_w16(obj + O_H_40, (uint16_t)v);
      if (v == -1) {
        c->mem_w8(obj + O_SUB_5, (uint8_t)(st + 1u));
        c->mem_w8(G_800BFA50, 0x0Au);
      }
      break;
    }
    case 8:
      if (c->mem_r8(G_800BFA50) != 0x14u) break;
      c->mem_w16(obj + O_H_40, 0u);
      c->mem_w8(obj + O_SUB_5, (uint8_t)(c->mem_r8(obj + O_SUB_5) + 1u));
      break;
    case 9: {
      // State 9 — additive-gray fade-out. Native ScreenFade path fires here for the FIRST TIME
      // since #27 was reported (was substrate FUN_8007E9C8 → dropped rect).
      const uint8_t s6 = c->mem_r8(obj + O_SUB_6);
      bool doneFinish = false;
      if (s6 == 1) {
        if (c->mem_r8(G_800BFA50) == 0x15u) c->mem_w8(obj + O_SUB_6, 2u);
      } else if (s6 == 0) {
        const int16_t v = (int16_t)(c->mem_r16(obj + O_H_40) + 8);
        c->mem_w16(obj + O_H_40, (uint16_t)v);
        if (v > 0xFF) {
          c->mem_w16(obj + O_H_40, 0xFFu);
          c->mem_w8(obj + O_SUB_6, (uint8_t)(c->mem_r8(obj + O_SUB_6) + 1u));
        }
      } else if (s6 == 2) {
        const uint16_t nv = (uint16_t)(c->mem_r16(obj + O_H_40) - 8u);
        c->mem_w16(obj + O_H_40, nv);
        if ((int32_t)((uint32_t)nv << 16) < 1) doneFinish = true;
      }
      if (!doneFinish) {
        const uint32_t u = c->mem_r8(obj + O_H_40);
        fade(eng(c).core).applyLeafCall((u << 16) | (u << 8) | u, /*ADDITIVE*/ 1u);
        return;
      }
      c->mem_w8(G_800BFA50, 0x16u);
      c->mem_w8(obj + O_STATE_4, 3u);
      return;
    }
    default:
      break;
  }

  // Tail (post-state-switch, states 2..8 fall through): obj[+1]=1; FUN_800517F8(obj); then a
  // conditional per-frame sprite draw gated on (*(u32)0x1F80017C & 3) == 0. The sprite is a
  // small icon whose id comes from *(u8)(DAT_80145A64 + obj[+0x42]) → table_base + id * 0x20.
  c->mem_w8(obj + O_FLAG_01, 1u);
  call1(c, obj, 0x800517F8u);
  if ((c->mem_r32(G_1F80017C) & 3u) == 0) {
    uint16_t h42 = c->mem_r16(obj + O_H_42);
    if (c->mem_r8(obj + O_SUB_5) > 6u || (int16_t)(h42 + 1) > 5) {
      h42 = 0u;
    } else {
      h42 = (uint16_t)(h42 + 1u);
    }
    c->mem_w16(obj + O_H_42, h42);
    // Recomp uses a local struct { u16 local_18=0x3F0, u16 local_16=0xE7, u16 local_14=0x10,
    // u16 local_12=1 } on stack; passes ptr and computed sprite arg.
    const uint32_t sp_save = c->r[29];
    c->r[29] = sp_save - 24u;
    const uint32_t ptr = c->r[29] + 6u;
    c->mem_w16(ptr + 0u, 0x03F0u);
    c->mem_w16(ptr + 2u, 0x00E7u);
    c->mem_w16(ptr + 4u, 0x0010u);
    c->mem_w16(ptr + 6u, 0x0001u);
    const uint32_t idIdx = (uint32_t)c->mem_r8(DAT_80145A64 + (uint32_t)c->mem_r16(obj + O_H_42));
    // (idIdx * 0x20) + (0x80145A64 - some absolute base). Recomp used the literal
    // 0x7FEBA61C = -0x8014_59E4 sign-flipped, so effective base = 0x8014_59E4.
    const uint32_t spriteArg = idIdx * 0x20u - 0x7FEBA61Cu;
    c->r[4] = ptr; c->r[5] = spriteArg;
    rec_dispatch(c, 0x80081218u);
    c->r[29] = sp_save;
  }
  (void)fellThrough;
}

// ── State 0 (INIT) — variant switch ─────────────────────────────────────────────────────────────
enum { LAB_bc0, LAB_b88, LAB_none };
inline int state0Case_5_or_16(Core* c, uint32_t obj, uint32_t modelArg) {
  c->mem_w32(obj + O_D_3C, c->mem_r32(G_800ECF80));
  call3(c, obj, PTR_801486EC, modelArg, 0x80077b38u);
  c->mem_w8 (obj + O_B_0D, 0u);
  c->mem_w16(obj + O_H_5C, 0u);
  c->mem_w8 (obj + O_B_0B, 0x10u);
  c->mem_w16(obj + O_H_5A, 0u);
  c->mem_w8 (obj + O_B_47, 0u);
  return LAB_none;
}
int state0Init(Core* c, uint32_t obj) {
  const uint8_t v = c->mem_r8(obj + O_VARIANT);
  switch (v) {
    case 0: case 1: case 2: case 3: {
      const int16_t argH = (int16_t)c->mem_r16(DAT_801459AC + (uint32_t)v * 2u);
      call3(c, obj, 0x0Cu, (uint32_t)(int32_t)argH, 0x80051B70u);
      return LAB_b88;
    }
    case 4: {
      const uint8_t g = c->mem_r8(G_800BFA31);
      const uint32_t arg = (g < 4u) ? 0x13u : (g < 7u ? 0x14u : 0x15u);
      call3(c, obj, 0x0Cu, arg, 0x80051B70u);
      return LAB_b88;
    }
    case 5: {
      c->mem_w32(obj + O_D_3C, c->mem_r32(G_800ECF80));
      call3(c, obj, PTR_801486EC, 9u, 0x80077b38u);
      c->mem_w8 (obj + O_B_0B, 0x10u);
      c->mem_w16(obj + O_H_5C, 0u);
      c->mem_w16(obj + O_H_5A, 0u);
      c->mem_w8 (obj + O_B_47, 0u);
      c->mem_w8 (obj + O_B_08, 0xF0u);
      return LAB_none;
    }
    case 6:
      call3(c, obj, 0x0Cu, 0x20u, 0x80051B70u);
      if ((int8_t)c->mem_r8(G_800BF8E2) != -1) {
        c->mem_w16(obj + O_H_2E, 0x03FCu);
        c->mem_w16(obj + O_H_36, 0x07F0u);
      } else {
        c->mem_w8(obj + O_SUB_5, 10u);
      }
      return LAB_none;
    case 7:
      if ((int8_t)c->mem_r8(G_800BF8E2) != -1) { call3(c, obj, 0x0Cu, 0x21u, 0x80051B70u); return LAB_none; }
      break;
    case 8:
      if ((int8_t)c->mem_r8(G_800BF8E2) != -1) { call3(c, obj, 0x0Cu, 0x22u, 0x80051B70u); return LAB_none; }
      break;
    case 9:
      call3(c, obj, 0x0Cu, 0x1Fu, 0x80051B70u);
      call1(c, obj, 0x800517F8u);
      if ((int8_t)c->mem_r8(G_800BF8DF) == -1) c->mem_w8(obj + O_SUB_5, 1u);
      return LAB_none;
    case 10:
      if (c->mem_r8(G_800BFB22) == 0 && c->mem_r32(G_800ED098) > 1u) {
        call3(c, obj, 0x0Cu, 0x4Du, 0x80051B70u);
        call5(c, obj, DAT_801459BC, 4u, 0x0Cu, 0x4Eu, 0x801268D8u);
        c->mem_w8 (obj + O_KIND, 1u);
        c->mem_w16(obj + O_H_80, 0xF8u);
        c->mem_w16(obj + O_H_82, 0x1F0u);
        c->mem_w16(obj + O_H_84, 0x220u);
        c->mem_w16(obj + O_H_86, 0x220u);
        c->mem_w8 (obj + O_B_29, 0u);
        c->mem_w8 (obj + O_B_5F, 0u);
        call1(c, obj, 0x8004766Cu);
        call1(c, obj, 0x80048750u);
        c->mem_w16(obj + O_H_60, c->mem_r16(G_1F8001A0));
        call1(c, obj, 0x800517F8u);
        const uint32_t ret = call3Ret(c, 0x90Bu, obj + O_ANIM_2C, (uint32_t)(int32_t)-0x20, 0x8003116Cu);
        c->mem_w32(obj + O_D_10, ret);
        return LAB_none;
      }
      break;
    case 0xB:
      if (call3Ret(c, obj, 0x0Cu, 0x6Cu, 0x80051B70u) == 0) {
        c->mem_w16(obj + O_H_56, 0x0ACDu);
        c->mem_w16(obj + O_H_58, 0x0C5Cu);
        c->mem_w16(obj + O_H_2E, 0x224Du);
        c->mem_w16(obj + O_H_32, 0xEA80u);
        c->mem_w16(obj + O_H_54, 0u);
        c->mem_w16(obj + O_H_36, 0x325Eu);
      }
      return LAB_none;
    case 0xE:
      if (call3Ret(c, obj, 0x0Cu, 0x2Bu, 0x80051B70u) == 0) return LAB_b88;
      return LAB_none;
    case 0x10:
      if (call3Ret(c, obj, 0x0Cu, 0x58u, 0x80051B70u) == 0) {
        c->mem_w8(obj + O_B_0D, (uint8_t)(c->mem_r8(obj + O_B_0D) | 4u));
        c->mem_w8(c->mem_r32(obj + O_D_C0) + 0x3Fu, 0x1Eu);
        c->mem_w8 (obj + O_KIND, 1u);
        c->mem_w16(obj + O_H_80, 0x78u);
        c->mem_w16(obj + O_H_82, 0xF0u);
        c->mem_w16(obj + O_H_84, 0x1C0u);
        c->mem_w16(obj + O_H_86, 0x1C0u);
        // Guest calls FUN_800517F8() with no obj arg (bug in recomp? — kept faithfully).
        rec_dispatch(c, 0x800517F8u);
      }
      return LAB_none;
    case 0x11:
      if (call3Ret(c, obj, 0x0Cu, 0x57u, 0x80051B70u) == 0) {
        if ((int8_t)c->mem_r8(G_800BF928) == -1) {
          c->mem_w8(obj + O_SUB_5, 1u);
          c->mem_w16(obj + O_H_2E, 0x40C9u);
          c->mem_w16(obj + O_H_32, 0xF148u);
          c->mem_w16(obj + O_H_36, 0x10EDu);
        } else {
          c->mem_w8(obj + O_SUB_5, 0u);
        }
        c->mem_w16(obj + O_H_54, 0u);
        c->mem_w16(obj + O_H_56, 0u);
        c->mem_w16(obj + O_H_58, 0u);
      }
      return LAB_none;
    case 0x12:
      return state0Case_5_or_16(c, obj, 0x0Cu);   // uVar4=0xC, joined path
    case 0x13:
      if (call3Ret(c, obj, 0x0Cu, 0x6Bu, 0x80051B70u) != 0) return LAB_none;
      c->mem_w16(obj + O_H_56, 0x04FCu);
      c->mem_w16(obj + O_H_58, 0u);
      c->mem_w16(obj + O_H_54, 0u);
      c->mem_w16(obj + O_H_40, 0u);
      c->mem_w32(obj + O_D_10, 0u);
      if ((int8_t)c->mem_r8(G_800BF93E) == -1) {
        c->mem_w8(obj + O_SUB_5, 10u);
        c->mem_w8(obj + O_B_0D, (uint8_t)(c->mem_r8(obj + O_B_0D) | 4u));
        c->mem_w8(c->mem_r32(obj + O_D_C0) + 0x3Fu, 0xF6u);
      }
      return LAB_none;
    case 0x14:
      if (call3Ret(c, obj, 0x0Cu, 0x6Eu, 0x80051B70u) == 0) {
        c->mem_w16(obj + O_H_58, 0u); c->mem_w16(obj + O_H_56, 0u); c->mem_w16(obj + O_H_54, 0u);
      }
      return LAB_none;
    case 0x15:
      if (call3Ret(c, obj, 0x0Cu, 0x6Du, 0x80051B70u) == 0) {
        c->mem_w16(obj + O_H_58, 0u); c->mem_w16(obj + O_H_56, 0u); c->mem_w16(obj + O_H_54, 0u);
      }
      return LAB_none;
    case 0x16:
      return state0Case_5_or_16(c, obj, 5u);
    case 0x17:
      if (c->mem_r8(G_800BFA42) == 0) { call3(c, obj, 0x0Cu, 0x6Cu, 0x80051B70u); return LAB_none; }
      break;
    case 0x18:
      c->mem_w8(obj + O_B_0B, 0x20u);
      c->mem_w32(obj + O_D_18, FN_8011973C);
      c->mem_w16(obj + O_H_2E, 0x3053u);
      c->mem_w16(obj + O_H_32, 0xEEEAu);
      c->mem_w16(obj + O_H_36, 0x17C3u);
      c->mem_w16(obj + O_H_84, 200u);
      c->mem_w16(obj + O_H_80, 0u);
      c->mem_w16(obj + O_H_82, 0u);
      c->mem_w16(obj + O_H_86, 400u);
      return LAB_none;
    case 0x19:
      call1(c, obj, 0x801279DCu); return LAB_none;
    case 0x1A:
      call1(c, obj, 0x80127B34u); return LAB_none;
    case 0x1B:
      c->mem_w8(obj + O_KIND, 1u);
      c->mem_w16(obj + O_H_80, 0x40u);
      c->mem_w16(obj + O_H_82, 0x80u);
      c->mem_w16(obj + O_H_84, 400u);
      c->mem_w16(obj + O_H_86, 800u);
      return LAB_none;
    default:
      return LAB_none;
  }
  return LAB_bc0;   // recomp: `goto LAB_80128bc0` for cases that "break" without a `return LAB_none`
}

// ── State 1 (RUN) — variant switch ──────────────────────────────────────────────────────────────
enum { RET_none, RET_b88, RET_b84, RET_bc0, RET_caseD_10 };
int state1Run(Core* c, uint32_t obj) {
  const uint8_t v = c->mem_r8(obj + O_VARIANT);
  switch (v) {
    case 0: case 1: case 2: case 3: case 4: {
      const int16_t sub = (int16_t)(((int32_t)((uint32_t)c->mem_r16(G_1F8000E2) - (uint32_t)c->mem_r16(obj + O_H_32)) << 16) >> 16);
      c->r[4] = obj; c->r[5] = (uint32_t)(int32_t)sub;
      rec_dispatch(c, 0x800778E4u);
      return RET_none;
    }
    case 5:
      if (c->mem_r8(G_800BFA4E) != 1u) {
        const uint32_t d10 = c->mem_r32(obj + O_D_10);
        call5(c, obj, 0u, c->mem_r32(d10 + 0xD8u), c->mem_r32(d10 + 0xCCu), 0u, 0x8004BD64u);
        c->mem_w8(obj + O_FLAG_01, 1u);
        return RET_none;
      }
      call2(c, 0x54u, 1u, 0x8004D4C4u);
      call1(c, obj, 0x8004B0D8u);
      return RET_bc0;
    case 6: call1(c, obj, 0x80126AD8u); return RET_none;
    case 7: call1(c, obj, 0x80126C68u); return RET_none;
    case 8: {
      const uint8_t s5 = c->mem_r8(obj + O_SUB_5);
      if (s5 == 0) {
        if (c->mem_r8(G_800BFA3B) == 0x0Eu) {
          c->mem_w8(obj + O_SUB_5, 1u);
          return RET_b84;
        }
      } else if (s5 != 1) {
        c->mem_w8(obj + O_FLAG_01, 1u);
        return RET_b88;
      } else {
        const int16_t s32 = (int16_t)c->mem_r16(obj + O_H_32);
        const int16_t ns = (int16_t)(s32 + 0x10);
        c->mem_w16(obj + O_H_32, (uint16_t)ns);
        if (ns > 499) {
          c->mem_w8(G_800BFA3B, 0x0Fu);
          c->mem_w8(obj + O_STATE_4, 3u);
          return RET_b84;
        }
      }
      return RET_b84;
    }
    case 9: {
      const uint8_t s5 = c->mem_r8(obj + O_SUB_5);
      if (s5 == 0) {
        if (c->mem_r8(G_800BFA3A) > 10u) { c->mem_w8(obj + O_SUB_5, 1u); }
        return RET_none;
      }
      if (s5 != 1) return RET_none;
      c->mem_w8(obj + O_FLAG_01, 1u);
      return RET_b88;
    }
    case 10: call1(c, obj, 0x80126E24u); return RET_none;
    case 0xB:
      if (c->mem_r8(G_800BF816) != 0) {
        if (call1Ret(c, obj, 0x8007778Cu) == 0) return RET_none;
        return RET_b88;
      }
      return RET_bc0;
    case 0xE: c->mem_w8(obj + O_FLAG_01, 1u); return RET_none;
    case 0x10: return RET_caseD_10;
    case 0x11:
      if (c->mem_r8(obj + O_SUB_5) == 0) {
        call5(c, obj, 0u, c->mem_r32(G_800E7F5C), c->mem_r32(G_800E7F50), 0u, 0x8004BD64u);
        if ((c->mem_r8(G_800BFA30) & 1u) != 0) {
          c->mem_w16(obj + O_H_2E, 0x40C9u);
          c->mem_w16(obj + O_H_32, 0xF148u);
          c->mem_w16(obj + O_H_36, 0x10EDu);
          c->mem_w8(obj + O_SUB_5, (uint8_t)(c->mem_r8(obj + O_SUB_5) + 1u));
        }
      }
      call1(c, obj, 0x800517F8u);
      return RET_caseD_10;
    case 0x12: {
      const uint32_t mask = c->mem_r8(G_800BFA32);
      const uint32_t shift = (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + O_H_60) & 0x1Fu;
      if (((mask >> shift) & 1u) == 0) {
        c->mem_w8(obj + O_FLAG_01, 1u);
        call1(c, obj, 0x8004DAECu);
        c->mem_w16(obj + O_H_2E, (uint16_t)(c->mem_r16(obj + O_H_2E) + 0x20u));
        return RET_none;
      }
      return RET_bc0;
    }
    case 0x13: call1(c, obj, 0x801273B4u); return RET_none;
    case 0x14:
      if (c->mem_r8(G_800BFA3A) != 0x0Bu) {
        c->mem_w8(obj + O_FLAG_01, 1u);
        return RET_b88;
      }
      return RET_bc0;
    case 0x15: return RET_b84;
    case 0x16: {
      const uint8_t s5 = c->mem_r8(obj + O_SUB_5);
      c->mem_w8(obj + O_FLAG_01, 1u);
      if (s5 == 1) {
        const uint32_t d10 = c->mem_r32(obj + O_D_10);
        const int32_t sum2E = (int32_t)c->mem_r32(c->mem_r32(d10 + O_D_D0) + O_ANIM_2C) +
                              (int32_t)c->mem_r32(c->mem_r32(d10 + O_D_DC) + O_ANIM_2C);
        c->mem_w16(obj + O_H_2E, (uint16_t)(sum2E / 2));
        const int32_t sum32 = (int32_t)c->mem_r32(c->mem_r32(d10 + O_D_D0) + 0x30u) +
                              (int32_t)c->mem_r32(c->mem_r32(d10 + O_D_DC) + 0x30u);
        c->mem_w16(obj + O_H_32, (uint16_t)((sum32 / 2) - 0x28));
        const int32_t sum36 = (int32_t)c->mem_r32(c->mem_r32(d10 + O_D_D0) + 0x34u) +
                              (int32_t)c->mem_r32(c->mem_r32(d10 + O_D_DC) + 0x34u);
        c->mem_w16(obj + O_H_36, (uint16_t)(sum36 / 2));
      } else if (s5 == 0) {
        call5(c, obj, 0u, c->mem_r32(G_800E7F5C), c->mem_r32(G_800E7F50), 0u, 0x8004BD64u);
      }
      return RET_none;
    }
    case 0x17: call1(c, obj, 0x801277E0u); return RET_none;
    case 0x18: {
      c->mem_w8(obj + O_FLAG_01, 1u);
      const uint8_t s5 = c->mem_r8(obj + O_SUB_5);
      if (s5 == 0) {
        if ((c->mem_r8(G_800BFA3F) & 0x80u) == 0) {
          int16_t s80 = (int16_t)c->mem_r16(obj + O_H_80);
          if (s80 != 0) c->mem_w16(obj + O_H_80, (uint16_t)((s80 - 1) - (s80 >> 2)));
        } else {
          c->mem_w8(obj + O_SUB_5, 1u);
        }
      } else if (s5 == 1) {
        if ((c->mem_r8(G_800BFA3F) & 0x80u) == 0) c->mem_w8(obj + O_SUB_5, 0u);
        else if ((int16_t)c->mem_r16(obj + O_H_80) < 0x100) {
          const int16_t s80 = (int16_t)c->mem_r16(obj + O_H_80);
          c->mem_w16(obj + O_H_80, (uint16_t)(s80 + 1 + ((0x100 - s80) >> 2)));
        }
      }
      return RET_none;
    }
    case 0x19: {
      if ((int8_t)c->mem_r8(G_800BF922) != -1) return RET_none;
      const uint8_t s5 = c->mem_r8(obj + O_SUB_5);
      if (s5 == 1) {
        const uint32_t c4 = c->mem_r32(obj + O_D_C4);
        const int16_t nv = (int16_t)(c->mem_r16(c4 + 8u) - 0x40);
        c->mem_w16(c4 + 8u, (uint16_t)nv);
        if (nv < -0x6FF) {
          c->mem_w8(obj + O_SUB_5, (uint8_t)(c->mem_r8(obj + O_SUB_5) + 1u));
          c->mem_w8(G_800BFA50, 0x1Fu);
          return RET_b84;
        }
      } else if (s5 == 0 && c->mem_r8(G_800BFA50) > 0x1Du) {
        c->mem_w8(obj + O_SUB_5, 1u);
        call3(c, 0x1Au, 0x1Bu, 0u, 0x80074590u);
      }
      return RET_b84;
    }
    case 0x1A:
      // THE FADE-CALLER — the whole reason this behavior is native. Reach the cutscene director
      // via a plain C call so its state-9 FUN_8007E9C8 becomes a native ScreenFade.
      cutsceneDirector(c, obj);
      return RET_none;
    case 0x1B:
      if ((int8_t)c->mem_r8(G_800BF8E3) != -1) {
        c->mem_w8(obj + O_FLAG_01, 1u);
        call1(c, obj, 0x80077EBCu);
        return RET_none;
      }
      return RET_bc0;
    default:
      return RET_none;
  }
}

}  // namespace

void beh_a08_scene_actor(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t state = c->mem_r8(obj + O_STATE_4);
  if (state == 1) {
    const int r = state1Run(c, obj);
    switch (r) {
      case RET_b84: c->mem_w8(obj + O_FLAG_01, 1u); [[fallthrough]];
      case RET_b88: call1(c, obj, 0x800517F8u); return;
      case RET_bc0: c->mem_w8(obj + O_STATE_4, 3u); return;
      case RET_caseD_10: call1(c, obj, 0x8007778Cu); return;
      default: return;
    }
  }
  if (state > 1) {
    if (state == 2) return;
    if (state == 3) { call1(c, obj, 0x8007A624u); return; }
    return;
  }
  if (state != 0) return;
  c->mem_w8(obj + O_STATE_4, 1u);
  const int r = state0Init(c, obj);
  if (r == LAB_b88) call1(c, obj, 0x800517F8u);
  else if (r == LAB_bc0) c->mem_w8(obj + O_STATE_4, 3u);
}
