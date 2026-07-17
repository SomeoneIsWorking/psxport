// game/ai/beh_a06_multi_actor.cpp — PC-native per-object behavior handler FUN_801189E8.
//
// A06 overlay's OMNIBUS scene-actor behaviour (~19 install sites across the overlay's object
// tables). Outer state machine on node[+4] (0 init / 1 active / 2 transition / 3 despawn), each
// wrapping an inner switch on node[+3] (~18 sub-types). Ported to close BUG-1 (cutscene fadeouts
// stuck black): case 10 of the state-1 body drives two per-frame FADE SUB-MACHINES via the guest
// leaf FUN_8007E9C8, which the still-recomp path routes to a substrate body that writes guest OT
// data our renderer no longer draws — so every fade rect from this handler is silently dropped
// and the ScreenFade HOLD latch at full-black never releases. Porting the parent behaviour +
// the two sub-machines native lands the fades on `fade(c).applyLeafCall(...)` where the
// present pipeline actually reads them.
//
// RE'd from Ghidra decomp of A06.BIN (scratch/ghidra/A06 project, base 0x80108F9C, imported
// 2026-07-03) — FUN_801189E8, FUN_801178A4, FUN_80117AAC. Ownership model matches the other
// beh_* handlers: CONTROL FLOW + node/global memory writes owned; every sub-behaviour leaf
// call stays a reachable substrate function via rec_dispatch (no recursion into leaves).
//
// Fade sub-machines detail:
//   whiteFlashPhaseRamp (guest FUN_801178A4): 5-state additive white flash ramp gated on the
//     external counter DAT_800BFA20. State 0 waits for counter>=2; state 1 pulses a mirror-
//     triangle gray LUT at 0x80144D58 (values 0x08→0x28→0x08 mod 8) while cycling node+0x40 on
//     odd frames; states 2/3/4 ramp node+0x40 upward by 8 per frame in three phases capped at
//     0x40 / 0x80 / 0xF8 respectively, each gated on the counter reaching 4/5/6. Phase 4
//     finalise: FUN_80051B04(node[+0xC0], 0xC, 0x49) music/SFX cue then reset state=0, advance
//     node+5.
//   whiteFadeHold (guest FUN_80117AAC): 3-state fade-in/out via ADDITIVE white. State 0 waits
//     for counter>=8; state 1 draws full 0xFFFFFF additive for 30 frames while ticking node+0x40
//     down, then advances (seeding node+0x40=0xF0); state 2 ramps additive gray DOWN by 0x20
//     per frame until node+0x40 < 0x21 then finalise: FUN_80051B04(node[+0xC0], 0xC, 0x48),
//     DAT_800BFA20 = 9, reset state, advance node+5.
//
// The a2=4 arg to FUN_8007E9C8 (OT slot index) is dropped — the native renderer no longer draws
// via a PSX OT.

#include "core.h"
#include "game_ctx.h"
#include "spawn.h"
#include "render/screen_fade.h"
#include "guest_abi.h"
#include <cstdint>

void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x801189E8u;

static inline uint32_t leaf_ret(Core* c) { return c->r[2]; }

// Guest addresses used as constants (each is a data pointer stored on the object or a callee).
constexpr uint32_t GFX_PTR_8014CEF0 = 0x8014CEF0u;   // A06 gfx-table for the humanoid-actor models (cases 0/1/7/0xB)
constexpr uint32_t GFX_PTR_80017334 = 0x80017334u;   // resident gfx-table (cases 0x10/0x11)
constexpr uint32_t LUT_80144D58     = 0x80144D58u;   // whiteFlashPhaseRamp color LUT (8 entries, mirror-triangle gray)
constexpr uint32_t GFX_PTR_8014D014 = 0x8014D014u;   // FUN_80040CDC arg1 (transition case)
constexpr uint32_t GFX_PTR_80144D28 = 0x80144D28u;   // FUN_80040CDC arg2 (transition case)

// FUN_801178A4 — the 5-phase white-flash phase ramp SM. See the file header for the phase
// gating; the four `func_0x8007e9c8(color, 1, 4)` calls now land on ScreenFade.
static void whiteFlashPhaseRamp(Core* c, uint32_t node) {
  uint8_t st = c->mem_r8(node + 6);
  switch (st) {
    case 0: {
      if (c->mem_r8(0x800BFA20u) < 2) return;
      c->mem_w16(node + 0x40, 0);
      c->mem_w8(node + 6, (uint8_t)(st + 1));
      return;
    }
    case 1: {
      uint16_t idx = c->mem_r16(node + 0x40);
      uint32_t color = c->mem_r32(LUT_80144D58 + (uint32_t)(int32_t)(int16_t)idx * 4u);
      fade(c).applyLeafCall(color, /*a1=ADDITIVE*/ 1);
      if ((c->mem_r32(0x1F80017Cu) & 1u) != 0) {
        c->mem_w16(node + 0x40, (uint16_t)((idx + 1u) & 7u));
      }
      if (c->mem_r8(0x800BFA20u) < 3) return;
      if ((int16_t)c->mem_r16(node + 0x40) != 0) return;
      c->mem_w16(node + 0x40, 0x20);
      c->mem_w8(node + 6, (uint8_t)(st + 1));
      return;
    }
    case 2: {
      uint32_t u = (uint32_t)c->mem_r8(node + 0x40);
      fade(c).applyLeafCall((u << 16) | (u << 8) | u, /*ADDITIVE*/ 1);
      int16_t v = (int16_t)c->mem_r16(node + 0x40);
      if (v < 0x40) { c->mem_w16(node + 0x40, (uint16_t)(v + 8)); return; }
      if (c->mem_r8(0x800BFA20u) < 4) return;
      c->mem_w8(node + 6, (uint8_t)(st + 1));
      return;
    }
    case 3: {
      uint32_t u = (uint32_t)c->mem_r8(node + 0x40);
      fade(c).applyLeafCall((u << 16) | (u << 8) | u, /*ADDITIVE*/ 1);
      int16_t v = (int16_t)c->mem_r16(node + 0x40);
      if (v < 0x80) { c->mem_w16(node + 0x40, (uint16_t)(v + 8)); return; }
      if (c->mem_r8(0x800BFA20u) < 5) return;
      c->mem_w8(node + 6, (uint8_t)(st + 1));
      return;
    }
    case 4: {
      uint32_t u = (uint32_t)c->mem_r8(node + 0x40);
      fade(c).applyLeafCall((u << 16) | (u << 8) | u, /*ADDITIVE*/ 1);
      int16_t v = (int16_t)c->mem_r16(node + 0x40);
      if (v < 0xF8) { c->mem_w16(node + 0x40, (uint16_t)(v + 8)); return; }
      if (c->mem_r8(0x800BFA20u) < 6) return;
      // Finalise: music/SFX cue then reset outer state, advance node+5.
      guest_leaf(c, 0x80051B04u, c->mem_r32(node + 0xC0), 0xC, 0x49);
      c->mem_w8(node + 6, 0);
      c->mem_w8(node + 5, (uint8_t)(c->mem_r8(node + 5) + 1));
      return;
    }
    default: return;   // st >= 5: guest falls through the switch to the epilogue; no state change.
  }
}

// FUN_80117AAC — 3-state fade-hold-fade-back companion to whiteFlashPhaseRamp.
static void whiteFadeHold(Core* c, uint32_t node) {
  uint8_t st = c->mem_r8(node + 6);
  if (st == 1) {
    fade(c).applyLeafCall(0x00FFFFFFu, /*ADDITIVE*/ 1);
    int16_t v = (int16_t)(c->mem_r16(node + 0x40) - 1);
    c->mem_w16(node + 0x40, (uint16_t)v);
    if (v == -1) {
      c->mem_w16(node + 0x40, 0xF0);
      c->mem_w8(node + 6, (uint8_t)(st + 1));
    }
    return;
  }
  if (st == 0) {
    if (c->mem_r8(0x800BFA20u) > 7) {   // matches guest `7 < DAT_800bfa20`
      c->mem_w8(node + 6, 1);
      c->mem_w16(node + 0x40, 0x1E);
    }
    return;
  }
  if (st == 2) {
    uint32_t u = (uint32_t)c->mem_r8(node + 0x40);
    fade(c).applyLeafCall((u << 16) | (u << 8) | u, /*ADDITIVE*/ 1);
    if ((int16_t)c->mem_r16(node + 0x40) < 0x21) {
      guest_leaf(c, 0x80051B04u, c->mem_r32(node + 0xC0), 0xC, 0x48);
      c->mem_w8(0x800BFA20u, 9);
      c->mem_w8(node + 6, 0);
      c->mem_w8(node + 5, (uint8_t)(c->mem_r8(node + 5) + 1));
    } else {
      c->mem_w16(node + 0x40, (uint16_t)((int16_t)c->mem_r16(node + 0x40) - 0x20));
    }
    return;
  }
  // st >= 3: no-op.
}

// ── outer state 0 — INIT (per node[+3]) ─────────────────────────────────────────────────────
// All init cases set node[+4]=1 up-front, then run a per-type prologue. Cases that fall through
// to the shared block at the switch tail (0/1/0x10/0x11 in the guest) additionally clear
// node[+0x5A]=0, node[+0x47]=0, node[+8]=0 at the end.
static void state0_init(Core* c, uint32_t nd) {
  c->mem_w8(nd + 4, 1);
  uint8_t n3 = c->mem_r8(nd + 3);
  bool shared_tail = false;
  switch (n3) {
    case 0: {
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      guest_leaf(c, 0x80077B38u, nd, GFX_PTR_8014CEF0, 5);
      c->mem_w8 (nd + 0x0D, 1);
      c->mem_w8 (nd + 0x0B, 0x11);
      c->mem_w16(nd + 0x7A, 0x1000);
      c->mem_w16(nd + 0x7C, 0x1000);
      c->mem_w16(nd + 0x7E, 0x1000);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w8 (nd + 0x08, 0xCE);
      return;
    }
    case 1:
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      guest_leaf(c, 0x80077B38u, nd, GFX_PTR_8014CEF0, 6);
      c->mem_w8 (nd + 0x0D, 1);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w8 (nd + 0x0B, 0x10);
      shared_tail = true;
      break;
    case 3: guest_leaf(c, 0x801168E4u, nd); return;
    case 4: guest_leaf(c, 0x80116D00u, nd); return;
    case 5:
      guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x26);
      c->mem_w16(nd + 0x54, 0);
      c->mem_w16(nd + 0x56, 0x800);
      c->mem_w16(nd + 0x58, 0);
      return;
    case 6: {
      guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x2A);
      c->mem_w16(nd + 0x56, 0x800);
      c->mem_w16(nd + 0x54, 0);
      c->mem_w16(nd + 0x58, 0);
      if (c->mem_r8(0x800BF8D5u) == 0xFF) {
        c->mem_w16(nd + 0x32, 0xDE88);
        c->mem_w8 (nd + 5, 99);
      }
      guest_leaf(c, 0x80072DDCu, nd, 1, 4, 0x17);
      uint32_t spawned = leaf_ret(c);
      if (spawned == 0) return;
      c->mem_w32(spawned + 0x1C, 0x80120FB4u);         // per-frame handler (kept substrate; installed at obj+0x1C)
      c->mem_w8 (spawned + 3, 0);
      c->mem_w8 (spawned + 0x28, (uint8_t)(c->mem_r8(spawned + 0x28) | 0x80));
      c->mem_w16(spawned + 0x2E, c->mem_r16(nd + 0x2E));
      c->mem_w16(spawned + 0x32, c->mem_r16(nd + 0x32));
      c->mem_w16(spawned + 0x36, c->mem_r16(nd + 0x36));
      return;
    }
    case 7:
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      guest_leaf(c, 0x80077B38u, nd, GFX_PTR_8014CEF0, 9);
      c->mem_w8 (nd + 0x0D, 3);
      c->mem_w8 (nd + 0x0B, 0x11);
      c->mem_w16(nd + 0x7C, 0x2000);
      c->mem_w8 (nd + 0x08, 0x30);
      c->mem_w8 (nd + 0x1A, 0x40);
      c->mem_w8 (nd + 0x19, 0x40);
      c->mem_w8 (nd + 0x18, 0x40);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w16(nd + 0x7A, 0x2800);
      c->mem_w16(nd + 0x7E, 0x2800);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w16(nd + 0x60, 0x96);
      return;
    case 8: guest_leaf(c, 0x80116FCCu, nd); return;
    case 9: {
      uint32_t sfx = (c->mem_r8(0x800BF8D3u) == 0xFF) ? 0x4Du : 0x4Cu;
      guest_leaf(c, 0x80051B70u, nd, 0x0C, sfx);
      c->mem_w16(nd + 0x56, 0xFDBC);
      guest_leaf(c, 0x800517F8u, nd);      // LAB_80119374 tail: obj-post-frame render helper
      return;
    }
    case 10: {
      // NB: the guest state-0 case-10 body handles the DAT_800bf921 / DAT_800bf922 gates + the
      // FUN_80141020 loop; it does NOT itself run the fade sub-machines. Faithful copy.
      if (c->mem_r8(0x800BF921u) != 0xFF) {
        guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x48);
        c->mem_w8(nd + 5, 0);
        int i = 0;
        uint8_t counter = c->mem_r8(0x800BFA21u);
        while (counter != 0) {
          if (i >= 10) break;
          guest_leaf(c, 0x80141020u, nd + 0x2C, 1, 0);
          i++;
          counter = (uint8_t)(i < (int)c->mem_r8(0x800BFA21u));
        }
        return;
      }
      if (c->mem_r8(0x800BF922u) != 0xFF && c->mem_r8(0x800BFB04u) == 0) {
        guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x49);
        c->mem_w8(nd + 5, 1);
        return;
      }
      guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x48);
      // Fall-through to LAB_8011906C = node[+5]=2, then shared tail.
      c->mem_w8(nd + 5, 2);
      shared_tail = true;
      break;
    }
    case 0xB:
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF80u));
      guest_leaf(c, 0x80077B38u, nd, GFX_PTR_8014CEF0, (uint32_t)(uint8_t)(c->mem_r8(nd + 0x5E) + 0x12));
      c->mem_w8 (nd + 0x0D, 1);
      c->mem_w8 (nd + 0x0B, 0x11);
      c->mem_w16(nd + 0x7A, 0x1000);
      c->mem_w16(nd + 0x7C, 0x1000);
      c->mem_w16(nd + 0x7E, 0x1000);
      c->mem_w8 (nd + 0x08, 0xFB);
      c->mem_w16(nd + 0x2E, 0x5D33);
      c->mem_w16(nd + 0x32, 0xE3E0);
      c->mem_w16(nd + 0x36, 0x412C);
      c->mem_w16(nd + 0x4A, 0xD800);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w16(nd + 0x50, 0x280);
      c->mem_w16(nd + 0x32, (uint16_t)((int16_t)c->mem_r16(nd + 0x32) + 100));
      return;
    case 0xC: guest_leaf(c, 0x80117BD4u, nd); return;
    case 0xD:
      guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x47);
      c->mem_w8(nd + 0x0D, (uint8_t)(c->mem_r8(nd + 0x0D) | 4));
      c->mem_w8(c->mem_r32(nd + 0xC0) + 0x3F, 0xF6);
      c->mem_w16(nd + 0x2E, 0x2380);
      c->mem_w16(nd + 0x32, 0xF38A);
      c->mem_w16(nd + 0x36, 0x34D4);
      return;
    case 0xE:
      guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x59);
      c->mem_w8(nd + 0x0D, (uint8_t)(c->mem_r8(nd + 0x0D) | 4));
      c->mem_w8(c->mem_r32(nd + 0xC0) + 0x3F, 0x20);
      guest_leaf(c, 0x800517F8u, nd);
      return;
    case 0xF: {
      uint32_t src = c->mem_r32(nd + 0x10);
      guest_leaf(c, 0x80051B70u, nd, 0x0C, 0x1C);
      c->mem_w16(nd + 0x54, 0);
      c->mem_w16(nd + 0x56, 0);
      c->mem_w16(nd + 0x58, 0);
      c->mem_w16(nd + 0x2E, c->mem_r16(src + 0x2E));
      c->mem_w16(nd + 0x32, (uint16_t)((int16_t)c->mem_r16(src + 0x32) - 0x50));
      c->mem_w16(nd + 0x36, c->mem_r16(src + 0x36));
      guest_leaf(c, 0x8004B354u, nd, 1);
      c->mem_w16(nd + 0xB8, 0);
      c->mem_w16(nd + 0xBA, 0);
      c->mem_w16(nd + 0xBC, 0);
      return;
    }
    case 0x10:
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF58u));
      guest_leaf(c, 0x80077B38u, nd, GFX_PTR_80017334, 0x17C);
      c->mem_w8 (nd + 0x0B, 0x11);
      c->mem_w8 (nd + 0x0D, 0);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w16(nd + 0x7A, 0x1400);
      c->mem_w16(nd + 0x7C, 0x1400);
      c->mem_w16(nd + 0x7E, 0x1400);
      shared_tail = true;
      break;
    case 0x11:
      c->mem_w32(nd + 0x3C, c->mem_r32(0x800ECF58u));
      guest_leaf(c, 0x80077B38u, nd, GFX_PTR_80017334, 0x17C);
      c->mem_w8 (nd + 0x0B, 0x11);
      c->mem_w8 (nd + 0x0D, 0);
      c->mem_w16(nd + 0x5C, 0);
      c->mem_w16(nd + 0x7A, 0x1400);
      c->mem_w16(nd + 0x7C, 0x1400);
      c->mem_w16(nd + 0x7E, 0x1400);
      c->mem_w16(nd + 0x5A, 0);
      c->mem_w8 (nd + 0x47, 0);
      c->mem_w8 (nd + 0x08, 0);
      c->mem_w8 (nd + 0x5E, c->mem_r8(nd + 0x2E));
      return;
    default: return;
  }
  if (shared_tail) {
    c->mem_w16(nd + 0x5A, 0);
    c->mem_w8 (nd + 0x47, 0);
    c->mem_w8 (nd + 8, 0);
  }
}

// ── outer state 1 — RUNNING (per node[+3]) ──────────────────────────────────────────────────
// The heavy state. Many cases end with the shared post-tail LAB_80119374 = FUN_800517F8(node)
// (obj-post-frame render helper — the OT-slot advance we no longer honour but keep the guest
// bookkeeping call so downstream leaves see the same node writes).
static void state1_run(Core* c, uint32_t nd) {
  uint8_t n3 = c->mem_r8(nd + 3);
  switch (n3) {
    case 0: {
      uint8_t s6 = c->mem_r8(nd + 6);
      if (s6 == 0) {
        c->mem_w8 (nd + 6, 1);
        c->mem_w16(nd + 0x50, 0);
      } else if (s6 == 1) {
        c->mem_w16(nd + 0x50, (uint16_t)((c->mem_r16(nd + 0x50) + 0x40u) & 0xFFFu));
        guest_leaf(c, 0x80083E80u);                 // returns v0
        int32_t r = (int32_t)leaf_ret(c);
        int16_t v = (int16_t)((r >> 2) + 0x1400);
        c->mem_w16(nd + 0x7A, (uint16_t)v);
        c->mem_w16(nd + 0x7E, (uint16_t)v);
        c->mem_w16(nd + 0x7C, (uint16_t)v);
      } else {
        c->mem_w8(nd + 1, 1);
        return;
      }
      c->mem_w8(nd + 1, 1);
      return;
    }
    case 1:
      guest_leaf(c, 0x80077B5Cu, nd);
      c->mem_w8(nd + 1, 1);
      return;
    case 3: guest_leaf(c, 0x80116AF8u, nd); return;
    case 4: {
      uint8_t s5 = c->mem_r8(nd + 5);
      if (s5 == 1) {
        if (c->mem_r8(0x800BFA22u) != 0) { c->mem_w8(nd + 5, 2); c->mem_w8(nd + 6, 0); return; }
        c->mem_w8(nd + 1, 1);
        guest_leaf(c, 0x800517F8u, nd);
        guest_leaf(c, 0x8004B374u, nd, 1);
      } else if (s5 == 2) {
        guest_leaf(c, 0x80116E48u, nd);
      } else if (s5 == 0) {
        if (c->mem_r8(0x800BFA22u) == 0) { c->mem_w8(nd + 5, 1); return; }
        // LAB_8011906C: node+5=2 then LAB_80119374 tail (postFrame render).
        c->mem_w8(nd + 5, 2);
        guest_leaf(c, 0x800517F8u, nd);
      }
      return;
    }
    case 5:
      c->mem_w8(nd + 1, 1);
      guest_leaf(c, 0x800517F8u, nd);              // LAB_80119374 tail
      return;
    case 6: {
      uint8_t s5 = c->mem_r8(nd + 5);
      if (s5 == 1) {
        int16_t v = (int16_t)c->mem_r16(nd + 0x4A);
        int16_t v2 = (int16_t)(v + 0x100);
        c->mem_w16(nd + 0x4A, (uint16_t)v2);
        c->mem_w32(nd + 0x30, c->mem_r32(nd + 0x30) + (uint32_t)((int32_t)v * -0x100));
        if (v2 > 0x1000) c->mem_w16(nd + 0x4A, 0x1000);
        if (c->mem_r8(0x800BFA22u) > 0x19) c->mem_w8(nd + 5, (uint8_t)(s5 + 1));
      } else if (s5 == 0 && c->mem_r8(0x800BFA22u) > 0x18) {
        c->mem_w8 (nd + 5, 1);
        c->mem_w16(nd + 0x4A, 0);
      }
      c->mem_w8(nd + 1, 1);
      guest_leaf(c, 0x800517F8u, nd);              // LAB_80119374 tail
      return;
    }
    case 7: {
      uint32_t src = c->mem_r32(nd + 0x10);
      uint8_t s5 = c->mem_r8(nd + 5);
      if (s5 == 0) {
        if (c->mem_r8(src + 0x5E) == 1) c->mem_w8(nd + 5, 1);
      } else if (s5 == 1) {
        c->mem_w16(nd + 0x60, (uint16_t)((int16_t)c->mem_r16(nd + 0x60) - 10));
        c->mem_w16(nd + 0x7C, (uint16_t)((int16_t)c->mem_r16(nd + 0x7C) - 0x200));
        c->mem_w8 (nd + 0x18, (uint8_t)(c->mem_r8(nd + 0x18) + 0x10));
        c->mem_w8 (nd + 0x19, (uint8_t)(c->mem_r8(nd + 0x19) + 0x10));
        c->mem_w8 (nd + 0x1A, (uint8_t)(c->mem_r8(nd + 0x1A) + 0x10));
        if (c->mem_r8(nd + 0x18) == 0) c->mem_w8(nd + 4, 3);
      }
      c->mem_w16(nd + 0x2E, c->mem_r16(src + 0x2E));
      c->mem_w16(nd + 0x32, (uint16_t)((int16_t)c->mem_r16(src + 0x32) - (int16_t)c->mem_r16(nd + 0x60)));
      c->mem_w16(nd + 0x36, c->mem_r16(src + 0x36));
      c->mem_w8 (nd + 1, c->mem_r8(src + 1));
      guest_leaf(c, 0x80077B5Cu, nd);
      return;
    }
    case 8: {
      // FUN_800778E4(nd, dy) with dy = ((DAT_1f8000e2 - node[+0x32]) sign-extend s16).
      int32_t dy = (int32_t)(int16_t)(c->mem_r16(0x1F8000E2u) - c->mem_r16(nd + 0x32));
      guest_leaf(c, 0x800778E4u, nd, (uint32_t)dy);
      uint8_t s5 = c->mem_r8(nd + 5);
      if (s5 == 1) {
        guest_leaf(c, 0x801174BCu, nd);
        if (leaf_ret(c) != 0) c->mem_w8(nd + 5, (uint8_t)(s5 + 1));
      } else if (s5 == 0) {
        guest_leaf(c, 0x80117290u, nd);
        if (leaf_ret(c) != 0) c->mem_w8(nd + 5, (uint8_t)(s5 + 1));
      } else if (s5 == 2) {
        guest_leaf(c, 0x801176D4u, nd);
        if (leaf_ret(c) != 0) { c->mem_w8(nd + 4, 2); c->mem_w8(nd + 5, 0); }
      }
      guest_leaf(c, 0x80051844u, nd);              // NodeXform::build (native — but we go via substrate here for now)
      return;
    }
    case 9:
      if (c->mem_r8(0x800BF8D3u) != 0xFF && (c->mem_r8(0x800BF8D3u) & 4) != 0) {
        c->mem_w8(0x800BF8D3u, (uint8_t)(c->mem_r8(0x800BF8D3u) & 0xFB));
        guest_leaf(c, 0x80051B04u, c->mem_r32(nd + 0xC0), 0x0C, 0x4D);
      }
      // Fallthrough to case 0xE (guest: `case 9: ... case 0xe:`).
      /* fallthrough */
    case 0xE:
      guest_leaf(c, 0x8007778Cu, nd);              // Actor::boundsCull-ish; leave substrate
      return;
    case 10: {
      uint8_t s5 = c->mem_r8(nd + 5);
      if (s5 == 1) {
        whiteFadeHold(c, nd);
      } else if (s5 == 0) {
        whiteFlashPhaseRamp(c, nd);
      }
      guest_leaf(c, 0x8007778Cu, nd);
      guest_leaf(c, 0x800517F8u, nd);              // LAB_80119374 tail
      return;
    }
    case 0xB: {
      c->mem_w8(nd + 1, 1);
      uint8_t s5 = c->mem_r8(nd + 5);
      uint32_t src = c->mem_r32(nd + 0x10);
      if (s5 == 1) {
        uint16_t s4a = (uint16_t)(c->mem_r16(nd + 0x4A) + c->mem_r16(nd + 0x50));
        c->mem_w16(nd + 0x4A, s4a);
        c->mem_w32(nd + 0x30, c->mem_r32(nd + 0x30) + (uint32_t)((int32_t)((uint32_t)s4a << 16) >> 8));
        uint32_t ref = c->mem_r32(c->mem_r32(src + 0xDC) + 0x30);
        if ((int32_t)(ref - 0x3C) < (int32_t)(int16_t)c->mem_r16(nd + 0x32)) {
          c->mem_w8(nd + 5, (uint8_t)(s5 + 1));
        }
      } else if (s5 == 0) {
        uint16_t s4a = (uint16_t)(c->mem_r16(nd + 0x4A) + c->mem_r16(nd + 0x50));
        c->mem_w16(nd + 0x4A, s4a);
        if ((int32_t)((uint32_t)s4a << 16) > 0) {
          c->mem_w16(nd + 0x50, (uint16_t)((int16_t)c->mem_r16(nd + 0x50) >> 2));
          c->mem_w8 (nd + 5, (uint8_t)(s5 + 1));
        }
        c->mem_w32(nd + 0x30, c->mem_r32(nd + 0x30) + (uint32_t)((int32_t)(int16_t)c->mem_r16(nd + 0x4A) * 0x100));
      } else if (s5 == 2) {
        uint32_t dc = c->mem_r32(src + 0xDC);
        c->mem_w16(nd + 0x2E, c->mem_r16(dc + 0x2C));
        c->mem_w16(nd + 0x32, (uint16_t)((int16_t)c->mem_r16(dc + 0x30) - 0xF));
        c->mem_w16(nd + 0x36, c->mem_r16(dc + 0x34));
      }
      return;
    }
    case 0xC: guest_leaf(c, 0x80117CF4u, nd); return;
    case 0xD: {
      guest_leaf(c, 0x8007778Cu, nd);
      if (leaf_ret(c) != 0) guest_leaf(c, 0x800517F8u, nd);
      if (c->mem_r8(0x800BF9D3u) == 6) {
        c->mem_w8(0x800BF9D3u, 7);
        guest_leaf(c, 0x80027144u, c->mem_r32(c->mem_r32(nd + 0xC0) + 0x40), nd + 0x2C, 0x700, 0x24);
        guest_leaf(c, 0x80074590u, 0xC, 0, 0);       // Sfx::trigger — but keep substrate for arg-ABI parity
        c->mem_w8(nd + 4, 3);
      }
      return;
    }
    case 0xF:  guest_leaf(c, 0x80117F34u, nd); return;
    case 0x10: guest_leaf(c, 0x801180A0u, nd); return;
    case 0x11: guest_leaf(c, 0x801188B0u, nd); return;
    default: return;
  }
}

// ── outer state 2 — TRANSITION sub-machine (guest: only node[+3]==8 does anything) ────────────
static void state2_transition(Core* c, uint32_t nd) {
  if (c->mem_r8(nd + 3) != 8) return;
  uint8_t s5 = c->mem_r8(nd + 5);
  if (s5 == 1) {
    guest_leaf(c, 0x8005308Cu);
    if (leaf_ret(c) == 0) return;
    c->mem_w8(nd + 5, (uint8_t)(s5 + 1));
    guest_leaf(c, 0x80042354u, 1, 1);
    c->mem_w8(0x800BFA1Du, (uint8_t)(c->mem_r8(0x800BFA1Du) | 1));
    guest_leaf(c, 0x80040CDCu, nd, GFX_PTR_8014D014, GFX_PTR_80144D28);
    c->mem_w8(nd + 0x70, 1);
  } else if (s5 == 2) {
    guest_leaf(c, 0x80041098u, nd);
    if (c->mem_r8(nd + 0x70) != 0xFF) return;
    c->mem_w8(nd + 4, 3);
    guest_leaf(c, 0x80042310u);
  } else if (s5 == 0) {
    if (c->mem_r8(0x800BF8D2u) != 0xFF) return;
    c->mem_w8(nd + 5, 1);
  }
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────────────────────
void beh_a06_multi_actor(Core* c) {
  uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);
  if (st == 1)      state1_run(c, nd);
  else if (st == 0) state0_init(c, nd);
  else if (st == 2) state2_transition(c, nd);
  else if (st == 3) eng(c).spawn.despawn(nd);       // FUN_8007A624
  // st >= 4: no-op
  (void)BEH_FN;
}
