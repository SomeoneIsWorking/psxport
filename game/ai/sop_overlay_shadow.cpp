// game/ai/sop_overlay_shadow.cpp — SOP intro-cutscene shared helpers FUN_8010AE30 + FUN_8010AB38.
//
// FUN_8010AE30 is the "one-shot SOP-overlay init helper" both `beh_sop_intro_pilot` (state_init) and
// `beh_sop_intro_lifted` (state_init) fire once their model attach succeeds (game/ai/beh_sop_intro_pilot.cpp,
// game/ai/beh_sop_intro_lifted.cpp). It spawns a pool-208 aux node (Spawn::dispatch(cls=0, type=6, list=1) —
// FUN_8007A980) and wires it up as a per-actor DROP-SHADOW quad:
//   node[+0x1C] = FUN_8010AB38   (this file's per-frame handler — the shadow's own tick)
//   node[+0x0B] = 0x20           (render/flag byte)
//   node[+0x10] = parent         (back-pointer to the actor that owns this shadow)
//   node[+0x18] = 0x8002AB5C     (raw guest fn-ptr value — the RESIDENT TERRAIN quad-draw routine
//                                 address, later-227's `terrain_render_pc`. Stored as DATA only: this
//                                 is the content-interface value the PSX per-object render walk reads
//                                 back as the shadow's draw callback. We do not call through it here —
//                                 CLAUDE.md: engine ownership stops at writing the correct content-
//                                 interface value, not re-deriving the render mechanism.)
//   node[+0x28] |= 0x80          (mark active/visible)
//
// FUN_8010AB38 is the shadow's own per-frame tick (state byte at node+4), read from node[+0x10] = the
// parent actor:
//   state 0 (spawn-frame): seed size/anim fields (node+0x40/0x42/0x54/0x56/0x58/0xE), advance to 1.
//   state 1 (running), gated on the shared SOP scene-beat byte (0x800BF9B4, same global
//     `beh_sop_intro_narration` gates its narration beat on) staying < 5:
//       node+1 mirrors the parent's "active" byte (parent+1); if nonzero, copy the parent's world
//       position (parent+0x2C/0x30/0x34) and two more fields (parent+0x2E -> node+0x48, parent+0x36 ->
//       node+0x4C) into the shadow, then compute node+0x4A = parent+0x32 + parent+0x84 (a Y-anchor +
//       elevation composite) and derive TWO ALPHA/SCALE RAMPS from the actor's jump elevation
//       (parent+0x84, isolated by the `+0x84` term cancelling the `+0x32` term already folded into
//       0x4A — see the arithmetic below): node+0x4E clamped [0,0x80], node+0x50 clamped [0,0x100]. Both
//       ramps shrink as elevation grows — a shadow that fades/shrinks the higher the actor jumps.
//       If the parent's active byte is 0, node+1 goes to 0 too (shadow hidden) but no ramp update.
//     Once the scene-beat reaches >=5, state advances to 2 (frozen — matches state_init/RUNNING no-op
//     once the narration beat takes over, `beh_sop_intro_narration`'s gate).
//   state 2: advance to 3 (despawn-pending).
//   state 3: despawn via the owned Spawn::despawn (FUN_8007A624).
//
// Ownership: CONTROL FLOW + ALL memory ops owned native, byte-exact to the Ghidra decompile
// (scratch/decomp/cluster1.c FUN_8010AE30, scratch/decomp/cluster2.c FUN_8010AB38). No sub-behavior
// calls in either body except the already-native Spawn::dispatch/despawn — this closes the LAST
// "FUN_8010AE30/8010B588 SOP helpers" pair CLAUDE.md's frontier note called out (0x8010B588, the
// "lifted" actor's own multi-state sub-tick, is a SEPARATE deeper state machine — left for its own
// pass, see docs/port-progress.md).
//
// Registered in BehaviorDispatch::kTable as `beh_sop_overlay_shadow` (0x8010AB38, SOP overlay) — a
// pc_skip=true-only shortcut like every other native `beh_*`; pc_faithful / SBS full always take the
// substrate body via rec_dispatch (BehaviorDispatch::dispatchObj's `!pc_skip` term), so this cannot
// affect the byte-exact gate.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "core/engine.h"   // eng(c).spawn
#include "spawn.h"          // Spawn::dispatch / despawn (FUN_8007A980 / FUN_8007A624, native)

namespace {
constexpr uint32_t SHADOW_HANDLER = 0x8010AB38u;   // this file's own per-frame tick (node+0x1C)
constexpr uint32_t SHADOW_DRAWFN  = 0x8002AB5Cu;   // resident terrain quad-draw addr (content-interface value; not called)
constexpr uint32_t SCENE_BEAT     = 0x800BF9B4u;   // shared SOP scene-beat byte
constexpr uint8_t  BEAT_LIMIT     = 5;             // ramp updates run while beat < 5 (matches narration's beat==5 gate)
}  // namespace

// FUN_8010AE30(parent) -> node ptr (0 on pool exhaustion).
uint32_t native_sop_overlay_shadow_spawn(Core* c, uint32_t parent) {   // FUN_8010AE30
  uint32_t node = eng(c).spawn.dispatch(/*cls=*/0, /*type=*/6, /*list=*/1);   // FUN_8007A980
  if (node == 0) return 0;

  c->mem_w32(node + 0x1Cu, SHADOW_HANDLER);
  c->mem_w8 (node + 0x0Bu, 0x20);
  c->mem_w32(node + 0x10u, parent);
  c->mem_w32(node + 0x18u, SHADOW_DRAWFN);
  c->mem_w8 (node + 0x28u, (uint8_t)(c->mem_r8(node + 0x28u) | 0x80u));
  return node;
}

// FUN_8010AB38(node) — per-frame tick, dispatched via node+0x1C.
void beh_sop_overlay_shadow(Core* c) {
  const uint32_t node   = c->r[4];
  const uint8_t  state  = c->mem_r8(node + 4u);
  const uint32_t parent = c->mem_r32(node + 0x10u);

  if (state == 1) {
    if (c->mem_r8(SCENE_BEAT) < BEAT_LIMIT) {
      const uint8_t parentActive = c->mem_r8(parent + 1u);
      c->mem_w8(node + 1u, parentActive);
      if (parentActive != 0) {
        c->mem_w16(node + 0x40u, 0x50);
        c->mem_w16(node + 0x42u, 0x6E);
        c->mem_w32(node + 0x2Cu, c->mem_r32(parent + 0x2Cu));
        c->mem_w32(node + 0x30u, c->mem_r32(parent + 0x30u));
        c->mem_w32(node + 0x34u, c->mem_r32(parent + 0x34u));
        c->mem_w16(node + 0x48u, c->mem_r16(parent + 0x2Eu));
        const int16_t anchorY = (int16_t)c->mem_r16(parent + 0x32u);
        const int16_t elev    = (int16_t)c->mem_r16(parent + 0x84u);
        const int16_t yAnchor = (int16_t)(anchorY + elev);
        c->mem_w16(node + 0x4Au, (uint16_t)yAnchor);
        c->mem_w16(node + 0x4Cu, c->mem_r16(parent + 0x36u));

        // Both ramps key off (yAnchor - anchorY) == elev (the anchorY term cancels — matches the
        // recomp's `*(param_1+0x4a) - *(iVar4+0x32)` reduction exactly since 0x4a was just stamped
        // anchorY+elev above).
        const int32_t diff = (int32_t)yAnchor - (int32_t)anchorY;   // == elev

        int16_t ramp80 = (int16_t)(0x80 - ((diff - 0x78) >> 2));
        c->mem_w16(node + 0x4Eu, (uint16_t)ramp80);
        if (ramp80 < 0)       { c->mem_w16(node + 0x4Eu, 0);      c->mem_w8(node + 1u, 0); }
        else if (ramp80 > 0x80) { c->mem_w16(node + 0x4Eu, 0x80); }

        int16_t ramp100 = (int16_t)(0x100 - ((diff - 0x78) >> 2));
        c->mem_w16(node + 0x50u, (uint16_t)ramp100);
        if (ramp100 < 0)        { c->mem_w16(node + 0x50u, 0);       c->mem_w8(node + 1u, 0); }
        else if (ramp100 > 0x100) { c->mem_w16(node + 0x50u, 0x100); }
      }
    } else {
      c->mem_w8(node + 4u, 2);
    }
  } else if (state < 2) {
    if (state == 0) {
      c->mem_w16(node + 0x40u, 0x50);
      c->mem_w8 (node + 4u,    1);
      c->mem_w16(node + 0x54u, 0);
      c->mem_w16(node + 0x42u, 0x6E);
      c->mem_w16(node + 0x0Eu, 0);
      c->mem_w16(node + 0x56u, 0);
      c->mem_w16(node + 0x58u, 0);
    }
  } else if (state == 2) {
    c->mem_w8(node + 4u, 3);
  } else if (state == 3) {
    eng(c).spawn.despawn(node);
  }
}
