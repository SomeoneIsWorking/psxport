// game/ai/beh_area_transition_machine.cpp — PC-native per-object BEHAVIOR handler FUN_80127798.
//
// Overlay handler (~x774/field-frame on seaside). THE area-transition / cutscene-fade driver: node[5]==3
// runs a node[6] phase machine that fades out, loads the next area (FUN_80054198/FUN_80054d14) and
// animates the transition camera; the common tail re-projects (FUN_8004bd64) + commits the camera
// (FUN_8006cba8). Owning this is required for (a) the native renderer's cutscene FADE to be driven
// PC-native and (b) a debug area-switch (the node[5]==3 path IS the switch-to-another-area mechanism).
//
//   STATE 0 (init @0x80127800): FUN_80051b70(node,0xc,0x52) gate; seed node[0x2e]/[0x32]/[0x36]/[0x58],
//     node[0xc0]->{0x38,0x3a,0x3c}=0x1000, node[0xb8..0xbc]=0x1400, box node[0x80..0x86], node[0x2a]=0x1d;
//     node[4]->1; FUN_80041194(node,0,0,0).
//   STATE 1 (idle @0x801278b0): proceed only if scratchpad[0x207] in [29,31] OR mem8(0x800bf9b5)==1, then
//     FUN_8007778c-gated FUN_80051844.
//   STATE 2 (machine @0x801278fc): node[5] sub-state — 2: re-arm (node[0/4/5], node[0x58]=0xe3); 1: derive
//     node[0x58] from node[0x62] (mem8(0x800e7fc7) picks raw vs 0x1000-delta&0xfff); 0/other: nothing;
//     3: the node[6] PHASE MACHINE (below). sub 0/1/2/other -> CD0 tail; sub 3 -> DAT tail -> CD0.
//   node[6] PHASE MACHINE (sub==3): 0 = start fade (DAT_800bf9b5=3, FUN_80042354(1,1), FUN_80040b48(0x42));
//     1 = wait DAT_800e7ea9; 2 = countdown then FIRE the area load (DAT_800e7e84/85/86, FUN_80054198 +
//     FUN_80054d14, set up the transition camera deltas via FUN_80085690 + /64 scaling into node[0x4e/50/52]
//     and the DAT_800e80xx block, FUN_80074590(0x25,0,0)); 3 = per-frame integrate the camera deltas into
//     DAT_800e7eac/eb0/eb4 + ramp node[0x4a]; 4 = re-seed DAT_800e7eac/eb0/eb4 hi-words + node[4]++.
//   STATE 3 (@0x80127cf0): FUN_8007a624(node).
//   CD0 tail: FUN_80051844(node); node[1]=1; FUN_80077e7c(node).
//   DAT tail (sub==3 only): clear scratchpad 0x1f8000c0..c4, FUN_8004bd64(node,0,*0x800e7f5c,same,&0x1f8000c0)
//     [5th arg STACKED at sp+0x10], FUN_8006cba8(&DAT_800e7eac).
//
// CONTROL FLOW + every node/global/scratchpad WRITE owned native byte-for-byte; every sub-behavior CALL
// stays a pure-PSX leaf via rec_dispatch. RE'd 1:1 from disas 0x80127798 (Ghidra decomp
// scratch/decomp/field2/80127798.c cross-checked — NB Ghidra wrongly flagged FUN_80054d14 "noreturn";
// it returns normally, so case-2's camera-setup tail after it IS reachable and is transcribed here).
// GOTCHAs: scratchpad[0x207] gate is `(val-29) < 3` UNSIGNED; case-4 writes are CONCAT22 = store the HIGH
// halfword (DAT+2); the camera deltas use `<<8` then signed `/64` (truncate toward 0, NOT >>6); s1's
// `<<16>>8` == (int16)s1<<8 (low-16 sign-extend); FUN_8004bd64's 5th arg is STACKED (mirror frame sp-0x38).
// The node[5]==3 transition path has no headless coverage (fires on area switch) — transcribed from disas
// + verifiable via the A/B gate the moment a transition is triggered. Byte-exact gate is the safety net.

#include "core.h"
#include "game_ctx.h"
#include "render/cull.h"    // Cull::enqueueQueueA (FUN_80077E7C)
#include "object/actor.h"    // Actor::boundsCull (FUN_8007778C native)
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"     // class Spawn (eng(c).spawn.despawn / dispatch / spawnAndInit)
#include "graphics_bind.h"   // ov_obj_record_init
#include "trig.h"    // class Trig — libgte ratan2
#include "camera/cutscene_camera.h"   // CutsceneCamera::runInitSeedGrp (was rec_dispatch 0x8006CBA8)
#include "render/render.h"   // rend(c)->mNodeXform (was rec_dispatch 0x80051844)
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x80127798u;

// DAT_800e7e80 struct base (s5) and the absolute fields used by the transition machine.
constexpr uint32_t G = 0x800E7E80u;
constexpr uint32_t G_eac = 0x800E7EACu, G_eb0 = 0x800E7EB0u, G_eb4 = 0x800E7EB4u; // camera pos (32b)
constexpr uint32_t G_eae = 0x800E7EAEu, G_eb2 = 0x800E7EB2u, G_eb6 = 0x800E7EB6u; // hi-words of above
constexpr uint32_t G_ea9 = 0x800E7EA9u, G_eaa = 0x800E7EAAu, G_ed6 = 0x800E7ED6u;
constexpr uint32_t G_e84 = 0x800E7E84u, G_e85 = 0x800E7E85u, G_e86 = 0x800E7E86u, G_fc7 = 0x800E7FC7u;
constexpr uint32_t G_f5c = 0x800E7F5Cu;
constexpr uint32_t G_806c = 0x800E806Cu, G_8074 = 0x800E8074u, G_8076 = 0x800E8076u, G_8078 = 0x800E8078u;

static inline int16_t s16(Core* c, uint32_t a) { return c->mem_r16s(a); }

static void cd0_tail(Core* c, uint32_t nd) {           // @0x80127cd0
  rend(c)->mNodeXform.build(nd);                              // was rec_dispatch 0x80051844
  c->mem_w8(nd + 1, 1);
  eng(c).cull.enqueueQueueA(nd);                    // FUN_80077E7C (native; return ignored)
}

static void dat_tail(Core* c, uint32_t nd) {           // @0x80127c9c (sub==3 only)
  c->mem_w16(0x1F8000C0u, 0);
  c->mem_w16(0x1F8000C2u, 0);
  c->mem_w16(0x1F8000C4u, 0);
  uint32_t a2 = c->mem_r32(G_f5c);                     // *PTR_DAT_800e7f5c
  uint32_t fsp = c->r[29] - 0x38;                      // mirror the recomp frame (prologue sp-=0x38)
  c->mem_w32(fsp + 0x10, 0x1F8000C0u);                 // FUN_8004bd64's 5th arg, stacked at sp+0x10
  c->r[4] = nd; c->r[5] = 0; c->r[6] = a2; c->r[7] = a2;
  uint32_t save = c->r[29]; c->r[29] = fsp;
  eng(c).graphicsBind.posCompose();                               // FUN_8004bd64(node,0,*0x800e7f5c,same,&0x1f8000c0)
  c->r[29] = save;
  CutsceneCamera(c, CutsceneCamera::CAM_OBJ).initSeedGrp(G_eac);   // FUN_8006cba8(&DAT_800e7eac) — native
}

// node[6] phase machine (node[5]==3). Mutates node + DAT_800e* state; all cases converge to the DAT tail.
static void node6_phase(Core* c, uint32_t nd) {
  uint8_t ph = c->mem_r8(nd + 6);
  switch (ph) {
    case 0:                                            // @0x801279b8 — start fade
      c->r[4] = 1; c->r[5] = 1; rec_dispatch(c, 0x80042354u);   // FUN_80042354(1,1)
      c->mem_w8(0x800BF9B5u, 3);                       // (v0=3 from the call's delay-slot store)
      eng(c).sceneEvents.arm(0x42);                 // area-transition event; FUN_80040B48 (native)
      c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
      break;
    case 1:                                            // @0x801279e4 — wait for DAT_800e7ea9
      if (c->mem_r8(G_ea9) != 0) {
        c->mem_w16(nd + 0x40, 6);
        c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
      }
      break;
    case 2: {                                          // @0x80127a08 — countdown, then FIRE area load
      int16_t before = s16(c, nd + 0x40);
      c->mem_w16(nd + 0x40, (uint16_t)(c->mem_r16(nd + 0x40) - 1));
      if (before != 1) break;                          // only fire when the counter hits 1->0
      c->mem_w8(G_e84, 4);
      c->mem_w8(G_e85, 0x21);
      c->mem_w8(G_e86, 0);
      c->mem_w8(G_ea9, 0);
      c->mem_w8(G_fc7, 1);
      eng(c).sceneTransition.clearSwapBlock(G);     // FUN_80054198 (native)
      c->r[4] = G; c->r[5] = 0x71; c->r[6] = 8; rec_dispatch(c, 0x80054D14u);  // FUN_80054d14(&DAT_800e7e80,0x71,8)
      // --- camera-transition setup (reachable; Ghidra's "noreturn" on FUN_80054d14 was wrong) ---
      c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
      int s2 = (int16_t)(8268  - c->mem_r16(G_eb6));   // (int16)(0x204c - hi(eb4))
      int s0 = (int16_t)(18867 - c->mem_r16(G_eae));   // (int16)(0x49b3 - hi(eac))
      int s1 = (int)(-1388 - (int)c->mem_r16(G_eb2));  // -1388 - mem16(eb2) (NOT yet 16-bit-clamped)
      uint32_t ang = (uint32_t)trigOf(c).ratan2(-s2, s0);   // FUN_80085690(-s2, s0) -> angle
      int s0d = (s0 << 8) / 64;                         // <<8 then signed /64 (truncate toward 0)
      int s1d = ((int16_t)s1 << 8) / 64;               // (s1<<16>>8)/64 == ((int16)s1<<8)/64
      int s2d = (s2 << 8) / 64;
      c->mem_w16(G_ed6, (uint16_t)((ang + 2048) & 0xfff));
      c->mem_w16(nd + 0x40, 64);
      c->mem_w16(nd + 0x4a, (uint16_t)(int16_t)-12288);
      c->mem_w16(nd + 0x4e, (uint16_t)s0d);
      c->mem_w16(nd + 0x50, (uint16_t)s1d);
      c->mem_w16(nd + 0x52, (uint16_t)s2d);
      c->mem_w16(G_8076, (uint16_t)(int16_t)-256);
      c->mem_w8 (G_806c, 1);                            // s3 = 1
      c->mem_w16(G_8078, 0);
      c->mem_w16(G_8074, (uint16_t)(int16_t)-1400);
      eng(c).sfx.trigger(0x25, 0, 0);       // FUN_80074590 (native)
      break;
    }
    case 3: {                                          // @0x80127ba0 — integrate camera deltas
      if (s16(c, nd + 0x40) == 0) {
        c->mem_w8(nd + 6, (uint8_t)(c->mem_r8(nd + 6) + 1));
        c->mem_w16(G_8078, 0x800);
      } else {
        c->mem_w16(nd + 0x40, (uint16_t)(c->mem_r16(nd + 0x40) - 1));
        c->mem_w32(G_eac, c->mem_r32(G_eac) + ((int32_t)s16(c, nd + 0x4e) << 8));
        c->mem_w32(G_eb0, c->mem_r32(G_eb0) + ((int32_t)s16(c, nd + 0x50) << 8));
        c->mem_w8 (G_ea9, 0);
        c->mem_w32(G_eb4, c->mem_r32(G_eb4) + ((int32_t)s16(c, nd + 0x52) << 8));
      }
      // common @0x80127c10: ramp node[0x4a] toward 0x3000
      c->mem_w16(nd + 0x4a, (uint16_t)(c->mem_r16(nd + 0x4a) + 0x180));
      if (s16(c, nd + 0x4a) > 0x3000) c->mem_w16(nd + 0x4a, 0x3000);
      c->mem_w32(G_eb0, c->mem_r32(G_eb0) + ((int32_t)s16(c, nd + 0x4a) << 8));
      break;
    }
    case 4:                                            // @0x80127c50 — re-seed hi-words, advance state
      c->mem_w8(0x800BF9B5u, 4);
      c->mem_w16(G_eae, 0x49b3);                        // CONCAT22: hi half of DAT_800e7eac
      c->mem_w16(G_eb2, 0xfa94);                        // hi half of DAT_800e7eb0
      c->mem_w16(G_eb6, 0x204c);                        // hi half of DAT_800e7eb4
      c->mem_w8(G_ea9, 1);
      c->mem_w8(G_eaa, 0x22);
      c->mem_w8(0x1F800207u, 0x22);
      c->mem_w8(nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));
      break;
    default:                                            // ph>=5 -> straight to the DAT tail
      break;
  }
}

}  // namespace

void beh_area_transition_machine(Core* c) {
  const uint32_t nd = c->r[4];
  uint8_t st = c->mem_r8(nd + 4);

  if (st == 1) {
    // ---------- STATE 1 (idle gate) ----------
    uint32_t d = (uint32_t)(c->mem_r8(0x1F800207u) - 29);          // scratchpad[0x207]
    if (d >= 3 && c->mem_r8(0x800BF9B5u) != 1) return;             // !((v-29)<3) && mem8(0x800bf9b5)!=1
    if (Actor(c, nd).boundsCull() == 0) return;                    // FUN_8007778C gate — Actor::boundsCull (native)
    rend(c)->mNodeXform.build(nd);                                        // was rec_dispatch 0x80051844
    return;
  }
  if (st >= 2) {
    if (st == 2) {
      // ---------- STATE 2 (the node[5] machine) ----------
      uint8_t sub = c->mem_r8(nd + 5);
      bool via_dat = false;
      if (sub == 2) {                                  // @0x80127970 — re-arm
        c->mem_w8(nd + 0, 1);
        c->mem_w8(nd + 4, 1);
        c->mem_w8(nd + 5, 0);
        c->mem_w16(nd + 0x58, 0xe3);
      } else if (sub == 1) {                           // @0x8012793c
        if (c->mem_r8(G_fc7) == 0)
          c->mem_w16(nd + 0x58, (uint16_t)((0x1000 - (int)s16(c, nd + 0x62)) & 0xfff));
        else
          c->mem_w16(nd + 0x58, c->mem_r16(nd + 0x62));
      } else if (sub == 3) {                           // @0x80127988 — the phase machine
        node6_phase(c, nd);
        via_dat = true;
      }
      // sub==0 or sub>=4: nothing, fall to CD0.
      if (via_dat) dat_tail(c, nd);
      cd0_tail(c, nd);
      return;
    }
    if (st == 3) { eng(c).spawn.despawn(nd); }   // STATE 3
    return;
  }
  if (st != 0) return;

  // ---------- STATE 0 (init) ----------
  c->r[4] = nd; c->r[5] = 0xc; c->r[6] = 0x52;
  eng(c).graphicsBind.recordInit();                        // FUN_80051b70(node, 0xc, 0x52)
  if (c->r[2] != 0) return;
  c->mem_w16(nd + 0x2e, 0x4f00);
  c->mem_w16(nd + 0x32, 0xed5e);
  c->mem_w16(nd + 0x36, 0x2a6c);
  c->mem_w16(nd + 0x58, 0xff1d);
  c->mem_w8 (nd + 0, 1);
  c->mem_w8 (nd + 0x29, 0);
  c->mem_w8 (nd + 4, (uint8_t)(c->mem_r8(nd + 4) + 1));  // node[4] 0 -> 1
  uint32_t sub = c->mem_r32(nd + 0xc0);
  c->mem_w16(sub + 0x38, 0x1000);
  c->mem_w16(sub + 0x3a, 0x1000);
  c->mem_w16(sub + 0x3c, 0x1000);
  c->mem_w16(nd + 0xb8, 0x1400);
  c->mem_w16(nd + 0xba, 0x1400);
  c->mem_w16(nd + 0xbc, 0x1400);
  c->mem_w16(nd + 0x80, 0x30);
  c->mem_w16(nd + 0x82, 0x60);
  c->mem_w16(nd + 0x84, 0x40);
  c->mem_w16(nd + 0x86, 0x40);
  c->mem_w8 (nd + 0x2a, 0x1d);
  c->r[4] = nd; c->r[5] = 0; c->r[6] = 0; c->r[7] = 0;
  rec_dispatch(c, 0x80041194u);                        // FUN_80041194(node, 0, 0, 0)
}
