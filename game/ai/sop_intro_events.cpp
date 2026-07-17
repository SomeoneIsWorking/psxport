// game/ai/sop_intro_events.cpp — native ports for a cluster of SOP intro-cutscene leaves in the
// 0x8010A000-0x8010CFFF band (wide-RE fleet wave, see docs/port-progress.md wave entry and
// docs/engine_re.md "SOP intro-cutscene scene actors"). §9 re-verified + wired 2026-07-10 (docs/
// fleet-workflow.md §9): every function's guest-stack frame (all six push one — the original draft
// omitted the mirror entirely) was cross-checked instruction-by-instruction against
// generated/ov_sop_shard_*.c and found otherwise byte-exact; the ONLY defect was the missing
// r16/r17/r31 spill/restore, now fixed per-function above. Wired via the shared override registry
// (RegisterSopIntroEventOverrides, bottom of this file, `overrides::install`) — sopBeatAdvanceWalk/
// Narration and sopIntroEffectTick/beh_orbit_spark_effect are reached only via rec_dispatch (no
// setter needed); sopOrbitPathStep/sopIntroEffectSpawn/sopLiftedSubtick have DIRECT intra-shard call
// sites in ov_sop_shard_*.c that bypass rec_dispatch, so those three also pass the ov_sop_set_override
// setter (same shape as game/ai/actor_melee_engage.cpp).
//
// Ghidra decomp source: `scratch/decomp/band_sop.c` (FUN_8010AF60/8010B078/8010B2D4/8010B588/8010BEAC),
// `scratch/decomp/band_sop2.c` (FUN_8010B44C), `scratch/decomp/band_sop4.c` (FUN_8010B11C) — imported
// from `scratch/bin/tomba2/ram_sop.bin` (the SOP intro-cutscene RAM dump), Ghidra project `ram_sop`.
// scratch/ is gitignored; regenerate with `tools/decomp.sh import scratch/bin/tomba2/ram_sop.bin
// ram_sop` then `tools/decomp.sh decomp ram_sop <out.c> list <addrs...>` if these files are gone.
//
// CONTEXT (confirmed, docs/engine_re.md "SOP intro-cutscene scene actors"): Sop::fieldMode spawns 3
// scene actors at sm[0x50]==0 LOAD — beh_sop_intro_pilot (0x8010ACFC, model 0x11), beh_sop_intro_lifted
// (0x8010B798, model 0x0F), beh_sop_intro_narration (0x8010B990, model 0x0F) — each already owned
// natively (game/ai/beh_sop_intro_*.cpp). All three read/write the shared SOP scene-beat byte
// `0x800BF9B4`. The functions in THIS file are the next layer down: sub-ticks/sub-motions/timers those
// three actors (mostly the "lifted" one) call into, which stayed substrate when those files landed.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "core/engine.h"          // eng(c).spawn / eng(c).placement / eng(c).script
#include "render/render.h"        // rend(c)->mNodeXform.buildWithOffset (FUN_800518FC)
#include "spawn.h"                 // Spawn::dispatch/despawn (native)
#include "world/placement.h"       // Placement::spawnWithParent (native, FUN_80072DDC)
#include "sop_intro_events.h"
void rec_dispatch(Core*, uint32_t);

namespace {
constexpr uint32_t SCENE_BEAT = 0x800BF9B4u;   // shared SOP scene-beat byte (docs/engine_re.md)
}  // namespace

// ===================================================================================================
// FUN_8010AF60 — sopBeatAdvanceWalk. CONFIDENCE: state-machine shape + every field write/call arg is a
// direct Ghidra transcription (HIGH). TRIGGER (corrected 2026-07-10, op05WaitFrames diagnosis): the
// "per-KEYFRAME ANIMATION-EVENT callback table" hypothesis below is FALSIFIED. 0x8010CA60/0x8010CA78/
// 0x8010CA90 are op-0x3E (call-fnptr) entries of the pilot's cutscene SCRIPT at 0x8010CA28 (seeded by
// beh_sop_intro_pilot's init via animEnvInit(obj, 0x80017FE8, 0x8010CA28) = ScriptInterp::init),
// stepped per-frame by ScriptInterp::step from beh_sop_intro_pilot::state_running — NOT an animation-
// event table, and NOT reached indirectly by the animation system. This address (0x8010AE9C, an
// address in the sop_overlay_shadow cluster) and 0x8010B078 (this file's other beat sequencer) also
// live as literal DATA at 0x8010CA60..0x8010CAAC (entries shaped {u16, addr32, u32, u32} every 0x18
// bytes), which is exactly the script's op-0x3E fnptr encoding. Both this function and
// sopBeatAdvanceNarration ARE registered (see RegisterSopIntroEventOverrides below) — the "Left
// UNWIRED" state is stale.
//
// Behavior (4-state timer sequencer on node+0x78 / node+0x42, byte-exact):
//   state 0: -> state 1, timer = 0x1E (30).
//   state 1: countdown; on expiry: SCENE_BEAT = 3, Engine::walkStart(node, 0xB4, 4), -> state 2,
//     timer = 0x28 (40).
//   state 2: countdown; on expiry: Engine::walkStart(node, 2, 6), -> state 3, timer = 0x1E (30).
//   state 3: countdown; on expiry: return 1 (done signal; state stays 3, no further writes).
//
// GUEST FRAME (2026-07-10 §9 re-verify fix): ov_sop_gen_8010AF60 pushes `addiu sp,-24` and spills
// s0(r16)/ra(r31) at sp+16/sp+20 before ANY body work — the original draft omitted this entirely.
// Mirror it: decrement r29, stash the incoming r16/r31 (they're callee-saved — Engine::walkStart
// below calls still-substrate leaves via rec_dispatch, which DOES clobber the shared r16/r31
// register file), restore both + r29 before every return so the caller's registers/guest-stack
// bytes come back byte-exact (docs/faithful-execution.md; game/ai/melee_proximity.cpp is the
// reference shape for a same-size frame).
uint32_t sopBeatAdvanceWalk(Core* c) {                          // FUN_8010AF60
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 24u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR31);

  const uint32_t node = c->r[4];
  uint8_t state = c->mem_r8(node + 0x78);
  uint32_t result = 0;

  if (state == 1) {
    int16_t timer = (int16_t)c->mem_r16(node + 0x42);
    c->mem_w16(node + 0x42, (uint16_t)(timer - 1));
    if (timer == 1) {
      c->mem_w8(SCENE_BEAT, 3);
      eng(c).walkStart(node, 0xB4u, 4);                      // FUN_80054D14
      c->mem_w16(node + 0x42, 0x28);
      c->mem_w8 (node + 0x78, (uint8_t)(c->mem_r8(node + 0x78) + 1));
    }
  } else if (state == 0) {
    c->mem_w8 (node + 0x78, 1);
    c->mem_w16(node + 0x42, 0x1E);
  } else if (state == 2) {
    int16_t timer = (int16_t)c->mem_r16(node + 0x42);
    c->mem_w16(node + 0x42, (uint16_t)(timer - 1));
    if (timer == 1) {
      eng(c).walkStart(node, 2u, 6);                         // FUN_80054D14
      c->mem_w16(node + 0x42, 0x1E);
      c->mem_w8 (node + 0x78, (uint8_t)(c->mem_r8(node + 0x78) + 1));
    }
  } else if (state == 3) {
    int16_t timer = (int16_t)c->mem_r16(node + 0x42);
    c->mem_w16(node + 0x42, (uint16_t)(timer - 1));
    if (timer == 1) result = 1;
  }

  c->r[31] = savedR31;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
  return result;
}

// ===================================================================================================
// FUN_8010B078 — sopBeatAdvanceNarration. CONFIDENCE: same shape as sopBeatAdvanceWalk (HIGH for the
// transcription; TRIGGER is the pilot's cutscene SCRIPT at 0x8010CA28, see the corrected note above —
// its address is an op-0x3E fnptr entry at 0x8010CA94, NOT an animation-event table). Sets
// SCENE_BEAT = 5 (the narration/void beat beh_sop_intro_narration gates its running body
// on) and re-snaps the BG transform block (GraphicsBind::setXformBlkBody with FIXED globals, not the
// calling node — a0=0x800E8008 the shared BG object per game/scene/sop.cpp:509-510, a1=0x8010C974 a
// SOP-overlay-resident transform-data blob) before a 10-frame hold that stamps a completion flag.
//
//   state 0: SCENE_BEAT = 5; GraphicsBind::setXformBlkBody(0x800E8008, 0x8010C974); timer = 10;
//     -> state 1.
//   state 1: countdown; on expiry: byte at (0x800BF80C + 3) = 1 (a flag byte within a shared dword);
//     return 1 (done). Otherwise return 0.
//
// GUEST FRAME (2026-07-10 §9 fix): same shape as sopBeatAdvanceWalk — ov_sop_gen_8010B078 pushes
// `addiu sp,-24` + s0/ra spills at sp+16/sp+20. Mirrored below (r16/r31 not touched by
// GraphicsBind::setXformBlk's own rec_dispatch(0x8006CBD0) call, but restore unconditionally to
// stay correct if that ever changes).
uint32_t sopBeatAdvanceNarration(Core* c) {                     // FUN_8010B078
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 24u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR31);

  const uint32_t node = c->r[4];
  uint8_t state = c->mem_r8(node + 0x78);
  uint32_t result = 0;

  if (state == 0) {
    c->mem_w8(SCENE_BEAT, 5);
    c->r[4] = 0x800E8008u; c->r[5] = 0x8010C974u;
    eng(c).graphicsBind.setXformBlk();                       // FUN_8006CBD0 (fixed globals, not `node`)
    c->mem_w16(node + 0x42, 10);
    c->mem_w8 (node + 0x78, (uint8_t)(c->mem_r8(node + 0x78) + 1));
  } else if (state == 1) {
    int16_t timer = (int16_t)c->mem_r16(node + 0x42);
    c->mem_w16(node + 0x42, (uint16_t)(timer - 1));
    if (timer == 1) {
      c->mem_w8(0x800BF80Cu + 3u, 1);
      result = 1;
    }
  }

  c->r[31] = savedR31;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
  return result;
}

// ===================================================================================================
// FUN_8010B11C — sopOrbitPathStep. CONFIDENCE HIGH (direct callers confirmed: FUN_8010B4F4 and
// FUN_8010B2F0 both `jal` it — neither is in this fleet's band, left un-RE'd; sopIntroEffectTick below
// is the one confirmed caller inside our band). A 4-state "one full revolution around a snapshot
// origin" motion: init snapshots node+0x2E/32/36 into node+0x90/92/94, then on every subsequent call
// (states 1 and 2 share the same per-tick body via the switch's fallthrough/goto) advances an
// elevation ramp (node+0xB8/BA/BC += 0x80 each call, uncapped 32-bit add; state bumps once the LOW
// HALFWORD of +0xB8 exceeds 0xFFF as UNSIGNED), then recomputes node+0x2E/32/36 from the snapshot,
// walks node+0x2C/0x34 (separate 32-bit fixed-point position fields) by rcos/rsin(phase)*0xA00, nudges
// +0x32 by -2, derives a facing angle via ratan2, and advances the phase (+0x4E, step 0x80) until it
// exceeds 0x1BFF (one full 0x1C00-unit revolution), at which point state advances again. State 3 resets
// state to 0, installs anim env (Animation::attach, obj, env=0x8001B860, mode=2) and returns 1 (the
// "orbit complete" signal sopIntroEffectTick polls for) — every other state/tick returns 0.
// GUEST FRAME (2026-07-10 §9 fix): ov_sop_gen_8010B11C pushes `addiu sp,-24` + s0(r16)/ra(r31)
// spills at sp+16/sp+20, same as the beat-advance pair above. The state==3 branch's
// rec_dispatch(0x80077C40) (Animation::attach, still substrate) DOES clobber the shared r16/r31
// register file internally, so restoring the SAVED incoming values (not whatever rec_dispatch left
// behind) is required, not optional. Restructured to a single exit point below so every return path
// shares the one epilogue.
uint32_t sopOrbitPathStep(Core* c) {                            // FUN_8010B11C
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 24u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR31);

  const uint32_t node = c->r[4];
  uint8_t state = c->mem_r8(node + 6);
  uint32_t result = 0;

  if (state == 3) {
    c->mem_w8(node + 6, 0);
    c->r[4] = node; c->r[5] = 0x8001B860u; c->r[6] = 2;
    rec_dispatch(c, 0x80077C40u);                                // Animation::attach (still substrate here)
    result = 1;
  } else if (state == 0 || state == 1 || state == 2) {
    if (state == 0) {
      c->mem_w8 (node + 6, 1);
      c->mem_w16(node + 0x4E, 0);
      c->mem_w16(node + 0x90, c->mem_r16(node + 0x2E));
      c->mem_w16(node + 0x92, c->mem_r16(node + 0x32));
      c->mem_w16(node + 0x94, c->mem_r16(node + 0x36));
    }

    // Shared per-tick body (states 0-after-init, 1, 2 all fall into this; state 2 skips straight here).
    if (state != 2) {
      int16_t b8 = (int16_t)c->mem_r16(node + 0xB8);
      int16_t ba = (int16_t)c->mem_r16(node + 0xBA);
      int16_t bc = (int16_t)c->mem_r16(node + 0xBC);
      c->mem_w16(node + 0xB8, (uint16_t)(b8 + 0x80));
      c->mem_w16(node + 0xBA, (uint16_t)(ba + 0x80));
      c->mem_w16(node + 0xBC, (uint16_t)(bc + 0x80));
      if (c->mem_r16(node + 0xB8) > 0x0FFFu) {                   // unsigned compare, faithful to the recomp
        c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));
      }
    }

    c->mem_w16(node + 0x2E, c->mem_r16(node + 0x90));
    c->mem_w16(node + 0x32, c->mem_r16(node + 0x92));
    c->mem_w16(node + 0x36, c->mem_r16(node + 0x94));

    int32_t phase = (int16_t)c->mem_r16(node + 0x4E);
    c->mem_w32(node + 0x2C, c->mem_r32(node + 0x2C) + trigOf(c).rcos(phase) * 0xA00);
    int32_t sinv = trigOf(c).rsin(phase);
    c->mem_w16(node + 0x32, (uint16_t)((int16_t)c->mem_r16(node + 0x32) - 2));
    c->mem_w32(node + 0x34, c->mem_r32(node + 0x34) + sinv * 0xA00);

    int16_t s94 = (int16_t)(c->mem_r16(node + 0x94) - 1);
    c->mem_w16(node + 0x94, (uint16_t)s94);
    int16_t s36 = (int16_t)(c->mem_r16(node + 0x36) - 1);
    c->mem_w16(node + 0x36, (uint16_t)s36);

    int32_t y = (int32_t)s36 - (int32_t)s94;
    int32_t x = (int32_t)(int16_t)c->mem_r16(node + 0x90) - (int32_t)(int16_t)c->mem_r16(node + 0x2E);
    c->mem_w16(node + 0x56, (uint16_t)trigOf(c).ratan2(y, x));

    int16_t phaseNext = (int16_t)(c->mem_r16(node + 0x4E) + 0x80);
    c->mem_w16(node + 0x4E, (uint16_t)phaseNext);
    if (phaseNext > 0x1BFF) {
      c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));
    }
  }
  // else: out-of-range state — matches recomp's implicit `return 0` (result stays 0)

  c->r[31] = savedR31;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
  return result;
}

// ===================================================================================================
// FUN_8010B44C — sopIntroEffectSpawn. CONFIDENCE HIGH: single caller FUN_8010BAF8 confirmed via xref
// (not in this fleet's band, un-RE'd — a script-VM-driven "spawn an orbiting effect + wait for beat 3"
// leaf that calls SCENE_BEAT-gated timers of its own; left for a future pass). Fully owned via the
// already-native Placement::spawnWithParent (FUN_80072DDC): spawn(a1=type=3, a2=class=3, a3=flag=0x1A)
// -> class==3 routes to Spawn::dispatch(cls=3,type=3,list=1); on success install sopIntroEffectTick
// as the child's node+0x1C handler (mirrors sop_overlay_shadow.cpp's SHADOW_HANDLER pattern) and
// re-stamp node+0x10 = parent (redundant with spawnWithParent's own write — faithful to the recomp,
// which re-writes it).
// GUEST FRAME (2026-07-10 §9 fix): ov_sop_gen_8010B44C pushes `addiu sp,-24` + s0(r16)/ra(r31)
// spills at sp+16/sp+20 around the Placement::spawnWithParent call (0x80072DDC, still substrate —
// clobbers the shared r16/r31 register file). Mirrored below.
uint32_t sopIntroEffectSpawn(Core* c) {                          // FUN_8010B44C
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 24u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR31);

  const uint32_t parent = c->r[4];
  c->r[4] = parent; c->r[5] = 3; c->r[6] = 3; c->r[7] = 0x1Au;
  eng(c).placement.spawnWithParent();
  const uint32_t node = c->r[2];
  if (node != 0) {
    c->mem_w32(node + 0x1Cu, /*sopIntroEffectTick*/ 0x8010B2D4u);
    c->mem_w32(node + 0x10u, parent);
  }

  c->r[31] = savedR31;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
  return node;
}

// ===================================================================================================
// FUN_8010B2D4 — sopIntroEffectTick. CONFIDENCE HIGH (byte-exact transcription; sole spawner is
// sopIntroEffectSpawn above, confirmed via xref). The child's per-frame tick, dispatched via its own
// node+0x1C (same mechanism as beh_sop_overlay_shadow). Model id 0xC (12) — a THIRD SOP-overlay model
// distinct from pilot (0x11) / lifted+narration (0x0F).
//
//   state 0 = INIT: model attach 0xC via GraphicsBind::recordArrayInit(node, 0xC,
//     *(u32*)0x800ECF98, 0x800A4BC8). On success: node+0x3C = *(u32*)0x800ECF9C, Animation::attach
//     (node, env=0x8001B860, mode=0), zero node+0xB8/BA/BC, -> state 1, node+0x32 -= 0x8C (Y lift).
//   state 1 = RUNNING: Cull::wrapFrame(node) (result unused, matches recomp); sub-state @node+5:
//     sub 1 -> ScriptInterp::step(node); if node+0x70 == 0xFF (-1 as u8) sub-state++;
//     sub 0 -> sopOrbitPathStep(node); once it signals done (returns 1): sub-state++, install the
//       ALTERNATE anim env (node, env=0x8001B860, data=0x8010CAB8) via ScriptInterp::init, node+0x70=1.
//     Always then: Engine::animTick(node); Engine::objMatrixCompose(node).
//   state 3 = DESPAWN: Spawn::despawn(node).
//   anything else: no-op (matches the recomp's `bVar1 != 2 && bVar1 == 3` guard shape).
// GUEST FRAME (2026-07-10 §9 fix): ov_sop_gen_8010B2D4 pushes `addiu sp,-32` + spills s1(r17)@sp+20
// (=node, held live across the whole function), ra(r31)@sp+24, s0(r16)@sp+16 (r16 is reused
// internally by the recomp as a scratch base pointer during the INIT branch; our native body never
// needs a persistent r16, so only the callee-save contract — preserve the CALLER's incoming r16 —
// matters here). Every `rec_dispatch` call inside (Cull::wrapFrame/recordArrayInit/Animation::attach)
// is still-substrate and clobbers the shared r16/r17/r31 register file, so restoring the SAVED
// incoming values before return is required.
void sopIntroEffectTick(Core* c) {                               // FUN_8010B2D4
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR17 = c->r[17];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 32u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR17);
  c->mem_w32(c->r[29] + 24u, savedR31);

  const uint32_t node = c->r[4];
  c->r[17] = node;                                 // gen: r17 = a0 live across the body — callee
                                                   // prologues (FUN_800519E0 etc.) spill it
  uint8_t state = c->mem_r8(node + 4);
  c->r[16] = state;                                // gen: r16 = state byte (live)

  if (state == 1) {
    c->r[4] = node; rec_dispatch(c, 0x8007778Cu);                // Cull::wrapFrame (still substrate here; result unused)

    uint8_t sub = c->mem_r8(node + 5);
    if (sub == 1) {
      eng(c).script.step(node);                                // FUN_80041098
      if ((int8_t)c->mem_r8(node + 0x70) == -1) {
        c->mem_w8(node + 5, (uint8_t)(sub + 1));
      }
    } else if (sub == 0) {
      c->r[4] = node;
      if (sopOrbitPathStep(c) != 0) {
        c->mem_w8(node + 5, (uint8_t)(sub + 1));
        eng(c).script.init(node, 0x8001B860u, 0x8010CAB8u);     // FUN_80040CDC = ScriptInterp::init
        c->mem_w8(node + 0x70, 1);
      }
    }
    (void)eng(c).animTick(node);                                // FUN_8004190C
    rend(c)->mNodeXform.buildWithOffset(node);                  // FUN_800518FC (NodeXform::buildWithOffset)
  } else if (state == 0) {
    c->r[16] = 0x800ECF58u;                                         // gen L_8010B32C: r16 = reloc base (live at the call)
    c->r[4] = node; c->r[5] = 0xCu; c->r[6] = c->mem_r32(0x800ECF98u); c->r[7] = 0x800A4BC8u;
    c->r[31] = 0x8010B348u;                                         // gen's return constant at this site
    rec_dispatch(c, 0x800519E0u);                                   // GraphicsBind::recordArrayInit (still substrate here)
    if (c->r[2] == 0) {
      c->mem_w32(node + 0x3Cu, c->mem_r32(0x800ECF9Cu));
      c->r[4] = node; c->r[5] = 0x8001B860u; c->r[6] = 0;
      rec_dispatch(c, 0x80077C40u);                                 // Animation::attach (still substrate here)
      c->mem_w16(node + 0xB8u, 0);
      c->mem_w16(node + 0xBAu, 0);
      c->mem_w16(node + 0xBCu, 0);
      c->mem_w8 (node + 4, (uint8_t)(c->mem_r8(node + 4) + 1));
      c->mem_w16(node + 0x32u, (uint16_t)((int16_t)c->mem_r16(node + 0x32u) - 0x8C));
    }
  } else if (state == 3) {
    eng(c).spawn.despawn(node);                                  // FUN_8007A624
  }
  // else: no-op

  c->r[31] = savedR31;
  c->r[17] = savedR17;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
}

// ===================================================================================================
// FUN_8010B588 — sopLiftedSubtick. CONFIDENCE HIGH (byte-exact transcription; called ONLY from
// FUN_8010B798 = beh_sop_intro_lifted's state_running, confirmed via xref — matches the LIVE code's own
// comment "0x8010B588, mirror of 0x8010696C-shaped, the lifted actor's OWN multi-state sub-tick").
// A 6-state SM (node+6) gated on SCENE_BEAT that progressively re-targets the LIFTED actor's own
// anim-table pointer (node+0x3C) and installs bigger scene-record sets as the intro beat advances.
//
//   state 0: install anim env (node, 0x80017FE8, data=0x8010CB80) [via the shared tail below], -> 1.
//   state 1 or 6: ScriptInterp::step(node); if node+0x70 != 0xFF (still animating) return (no state
//     change); else -> state+1 (NO anim-install — this path skips the tail).
//   state 2: gated on SCENE_BEAT==2; snap node+0x2E=0x1144, node+0x36=0x4AB5, node+0x56=0x500 (facing);
//     -> state+1 (no anim-install; shares state1/6's "advance only" tail).
//   state 3: gated on SCENE_BEAT==3; node+0x3C = *(u32*)0x800ECFA8 (narration's alt anim table);
//     Animation::attach(node, env=0x8010D39C, mode=2); GraphicsBind::installSceneRecord three times on
//     node+0xC4/0xD0/0xDC (each already a scene-record ptr) with (0x12, 0xF)/(0x12, 0x10)/(0x12, 0x11);
//     -> state+1 (shares the "advance only" tail with state1/6/2).
//   state 4: gated on node+0x79 == 1; -> state+1; Sfx::trigger(3,0,0); node+0x3C = *(u32*)0x800ECF68
//     (LIFTED's own anim table, matches beh_sop_intro_lifted's G_ANIM_TABLE); the 4-arg variant
//     FUN_80077CFC(node, env=0x80017FE8, 2, 6) — STILL SUBSTRATE (no native owner, codemap-checked);
//     GraphicsBind::installSceneRecord x3 on 0xC4/0xD0/0xDC with (0x12,1)/(0x12,4)/(0x12,7). Returns
//     immediately (does NOT fall into the shared install-anim-env tail — case4 is fully self-contained).
//   state 5: gated on SCENE_BEAT==6; snap node+0x2E=0x4010, node+0x32=0xF1BE(signed -3650),
//     node+0x36=0x4EB5, node+0x56=0x800; falls into the SHARED TAIL (install anim env, data=0x8010CBD0).
//   default (state > 5): no-op.
//
//   SHARED TAIL (states 0 and 5 only): state = state+1; ScriptInterp::init(node, 0x80017FE8, data);
//   node+0x70 = 1.
namespace {
// Body-only implementation (early-return-heavy switch) — wrapped below by sopLiftedSubtick's guest
// frame mirror. Same split as beh_pickup_collect_trigger.cpp's *_body() / wrapper shape.
void sopLiftedSubtickBody(Core* c) {
  const uint32_t node = c->r[4];
  const uint8_t  state = c->mem_r8(node + 6);

  uint32_t animData = 0;      // set by cases 0/5 -> shared tail below
  bool     runTail   = false;

  switch (state) {
    case 0:
      animData = 0x8010CB80u;
      runTail  = true;
      break;
    case 1:
    case 6: {
      c->r[31] = 0x8010B768u;                                      // gen call-site ra (see state 3)
      eng(c).script.step(node);                                 // FUN_80041098
      if ((int8_t)c->mem_r8(node + 0x70) != -1) return;
      c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));      // advance only, no anim-install
      return;
    }
    case 2: {
      if (c->mem_r8(SCENE_BEAT) != 2) return;
      c->mem_w16(node + 0x2Eu, 0x1144);
      c->mem_w16(node + 0x36u, 0x4AB5);
      c->mem_w16(node + 0x56u, 0x500);
      c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));      // advance only, no anim-install
      return;
    }
    case 3: {
      if (c->mem_r8(SCENE_BEAT) != 3) return;
      c->mem_w32(node + 0x3Cu, c->mem_r32(0x800ECFA8u));
      // r31 = gen call-site constants (ov_sop_gen_8010B588): every callee here spills the caller's
      // ra into its own guest frame (attach's frame mirror / FUN_80051B04's substrate prologue), so
      // a stale r31 lands as a real guest-stack byte diff (watch-cut f328, 0x801FE90C).
      c->r[4] = node; c->r[5] = 0x8010D39Cu; c->r[6] = 2;
      c->r[31] = 0x8010B644u;
      rec_dispatch(c, 0x80077C40u);                                 // Animation::attach (still substrate here)
      c->r[31] = 0x8010B654u;
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(node + 0xC4u), 0x12u, 0x0Fu);   // FUN_80051B04
      c->r[31] = 0x8010B664u;
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(node + 0xD0u), 0x12u, 0x10u);
      c->r[31] = 0x8010B674u;
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(node + 0xDCu), 0x12u, 0x11u);
      c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));      // advance only, no anim-install
      return;
    }
    case 4: {
      if (c->mem_r8(node + 0x79u) != 1) return;
      c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));
      c->r[31] = 0x8010B6A4u;                                        // gen call-site ra (see state 3)
      eng(c).sfx.trigger(3, 0, 0);                                // FUN_80074590
      c->mem_w32(node + 0x3Cu, c->mem_r32(0x800ECF68u));
      c->r[4] = node; c->r[5] = 0x80017FE8u; c->r[6] = 2; c->r[7] = 6;
      c->r[31] = 0x8010B6C8u;
      rec_dispatch(c, 0x80077CFCu);                                 // still substrate (no native owner)
      c->r[31] = 0x8010B6D8u;
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(node + 0xC4u), 0x12u, 1u);
      c->r[31] = 0x8010B6E8u;
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(node + 0xD0u), 0x12u, 4u);
      c->r[31] = 0x8010B6F8u;
      eng(c).graphicsBind.installSceneRecord(c->mem_r32(node + 0xDCu), 0x12u, 7u);
      return;
    }
    case 5:
      if (c->mem_r8(SCENE_BEAT) != 6) return;
      c->mem_w16(node + 0x2Eu, 0x4010);
      c->mem_w16(node + 0x32u, 0xF1BE);
      c->mem_w16(node + 0x36u, 0x4EB5);
      c->mem_w16(node + 0x56u, 0x800);
      animData = 0x8010CBD0u;
      runTail  = true;
      break;
    default:
      return;
  }

  if (runTail) {
    c->mem_w8(node + 6, (uint8_t)(c->mem_r8(node + 6) + 1));
    c->r[31] = 0x8010B754u;                                          // gen call-site ra (see state 3)
    eng(c).script.init(node, 0x80017FE8u, animData);              // FUN_80040CDC = ScriptInterp::init
    c->mem_w8(node + 0x70u, 1);
  }
}
}  // namespace

// GUEST FRAME (2026-07-10 §9 fix): ov_sop_gen_8010B588 pushes `addiu sp,-24` + s0(r16)/ra(r31)
// spills at sp+16/sp+20 (r16 = node, held live across the whole switch). The state==3/4 branches'
// rec_dispatch calls (Animation::attach, the 4-arg anim-attach variant) are still-substrate and
// clobber the shared r16/r31 register file, so this mirrors the frame around the whole body
// (early-return-heavy, factored into sopLiftedSubtickBody above so every return path still hits
// this one entry/exit).
void sopLiftedSubtick(Core* c) {                                  // FUN_8010B588
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR16 = c->r[16];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 24u;
  c->mem_w32(c->r[29] + 16u, savedR16);
  c->mem_w32(c->r[29] + 20u, savedR31);

  sopLiftedSubtickBody(c);

  c->r[31] = savedR31;
  c->r[16] = savedR16;
  c->r[29] = savedSp;
}

// ===================================================================================================
// FUN_8010BEAC — beh_orbit_spark_effect. CONFIDENCE: state-machine shape + field writes HIGH (direct
// Ghidra transcription). OWNERSHIP CONTEXT UNCONFIRMED: this address has exactly one xref in ram_sop.bin
// and it is NOT a `jal`/`j` call site — it is a raw 4-byte DATA reference at 0x800A22B8, inside a small
// MAIN.EXE-RESIDENT table (adjacent entries include 0x8010BF54, also SOP-overlay-local) that looks like
// a per-object-TYPE handler table (same shape as the class-indexed tables Spawn::dispatch's 5 variants
// use), not SOP-scene state. That means this is most likely a GENERIC reusable "orbiting spark" particle
// TYPE handler reachable from anywhere via the normal spawn/type-dispatch mechanism, not exclusive to
// the SOP intro — the actual spawner/table owner was not traced this pass (deferred; a future pass
// should Ghidra-xref 0x800A22B8 itself, or entity_walk.py-scan for the handler pointer, to find who
// installs it on a node's +0x1C).
//
//   state 0: -> state 1; node+0x48=0x400 (facing/scale?), node+0x4A=0, node+0x4C(u32)=0, node+0x50=0.
//   states 0(after init)/1: node+1=1 (active flag); node+0x4E -= 0x20 (short, orbit phase decrement);
//     node+0x50: v=old value, v2=v-9; store v2; if v2 is NEGATIVE as signed 16-bit, overwrite with
//     v+0x4B instead (wraparound — NOT v2+0x4B) — a bounded angular counter.
//   states 2/3: Spawn::despawn(node) (recomp shows the call with no visible arg — the real ABI is a0 =
//     node like every other despawn call in this codebase; matches eng(c).spawn.despawn(node)).
//   state > 3: no-op.
// GUEST FRAME (2026-07-10 §9 fix): ov_sop_gen_8010BEAC pushes `addiu sp,-24` + ra(r31) ONLY at
// sp+16 — unlike its five siblings above, this leaf never reuses r16 as a scratch/base register
// (it addresses everything off the live a0=r4 directly), so no s0 spill exists in the recomp
// prologue. Mirror just the ra spill/restore; restructured to a single exit so the early-return
// branches (state>3, state==2/3) share it.
void beh_orbit_spark_effect(Core* c) {                            // FUN_8010BEAC
  const uint32_t savedSp = c->r[29];
  const uint32_t savedR31 = c->r[31];
  c->r[29] -= 24u;
  c->mem_w32(c->r[29] + 16u, savedR31);

  const uint32_t node = c->r[4];
  uint8_t state = c->mem_r8(node + 4);
  bool active = true;

  if (state != 1) {
    if (state > 1) {
      if (state <= 3) eng(c).spawn.despawn(node);                 // FUN_8007A624
      active = false;
    } else if (state != 0) {
      active = false;
    } else {
      c->mem_w8 (node + 4, 1);
      c->mem_w16(node + 0x48u, 0x400);
      c->mem_w16(node + 0x4Au, 0);
      c->mem_w32(node + 0x4Cu, 0);
      c->mem_w16(node + 0x50u, 0);
    }
  }

  if (active) {
  c->mem_w8 (node + 1, 1);
  c->mem_w16(node + 0x4Eu, (uint16_t)((int16_t)c->mem_r16(node + 0x4Eu) - 0x20));

  uint16_t v  = c->mem_r16(node + 0x50u);
  int16_t  v2 = (int16_t)(v - 9u);
  c->mem_w16(node + 0x50u, (uint16_t)v2);
  if (v2 < 0) {
    c->mem_w16(node + 0x50u, (uint16_t)(v + 0x4Bu));
  }
  }  // if (active)

  c->r[31] = savedR31;
  c->r[29] = savedSp;
}

// ===================================================================================================
// Wiring (2026-07-10). `overrides::install` keeps SBS core B / MV_CHECK's substrate-replay
// leg running the literal ov_sop_gen_* body (the pure reference the native port is byte-compared
// against) via oracle-gated dispatch — same shape as game/ai/actor_melee_engage.cpp /
// game/ai/beh_actor_tomba_proximity_combat.cpp.
extern void ov_sop_gen_8010AF60(Core*);
extern void ov_sop_gen_8010B078(Core*);
extern void ov_sop_gen_8010B11C(Core*);
extern void ov_sop_gen_8010B2D4(Core*);
extern void ov_sop_gen_8010B44C(Core*);
extern void ov_sop_gen_8010B588(Core*);
extern void ov_sop_gen_8010BEAC(Core*);
// ov_sop_set_override: the setter passed to `overrides::install` for addresses with a DIRECT
// ov_sop_func_XXXX(c) call site inside ov_sop_shard_*.c (bypasses rec_dispatch, so installing
// without a setter would be invisible to that call shape) — sopOrbitPathStep/sopIntroEffectSpawn/
// sopLiftedSubtick only.
extern void ov_sop_set_override(uint32_t, void (*)(Core*));

namespace {
void ov_sopBeatAdvanceWalk(Core* c) {
  // op-0x3E fnptr callee: ScriptInterp::callFnptr consumes v0 as the script pause/advance code —
  // the wrapper MUST publish the return in r[2] like the gen tail does (0 running / 1 done).
  c->r[2] = sopBeatAdvanceWalk(c);
}
void ov_sopBeatAdvanceNarration(Core* c) { c->r[2] = sopBeatAdvanceNarration(c); }
void ov_sopOrbitPathStep(Core* c)        { c->r[2] = sopOrbitPathStep(c); }
void ov_sopIntroEffectTick(Core* c)      { sopIntroEffectTick(c); }
void ov_sopIntroEffectSpawn(Core* c)     { c->r[2] = sopIntroEffectSpawn(c); }
void ov_sopLiftedSubtick(Core* c)        { sopLiftedSubtick(c); }
void ov_behOrbitSparkEffect(Core* c)     { beh_orbit_spark_effect(c); }
}  // namespace

void RegisterSopIntroEventOverrides(Game* /*game*/) {
  using overrides::install;
  // Reached only via rec_dispatch (animation-event fn-ptr table / node+0x1C dispatch) — no direct
  // intra-shard call site, so setter omitted.
  install(0x8010AF60u, "sopBeatAdvanceWalk",      ov_sopBeatAdvanceWalk,      ov_sop_gen_8010AF60);
  install(0x8010B078u, "sopBeatAdvanceNarration", ov_sopBeatAdvanceNarration, ov_sop_gen_8010B078);
  install(0x8010B2D4u, "sopIntroEffectTick",      ov_sopIntroEffectTick,      ov_sop_gen_8010B2D4);
  install(0x8010BEACu, "beh_orbit_spark_effect",  ov_behOrbitSparkEffect,     ov_sop_gen_8010BEAC);
  // Direct intra-shard call sites (ov_sop_func_XXXX(c), bypass rec_dispatch) -> ov_sop_set_override
  // installs the thunk so those callers reach native too.
  install(0x8010B11Cu, "sopOrbitPathStep",        ov_sopOrbitPathStep,        ov_sop_gen_8010B11C, ov_sop_set_override);
  install(0x8010B44Cu, "sopIntroEffectSpawn",     ov_sopIntroEffectSpawn,     ov_sop_gen_8010B44C, ov_sop_set_override);
  install(0x8010B588u, "sopLiftedSubtick",        ov_sopLiftedSubtick,        ov_sop_gen_8010B588, ov_sop_set_override);
}
