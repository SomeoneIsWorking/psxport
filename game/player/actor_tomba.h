// class ActorTomba — the PC-native Tomba actor: a NAMED-FIELD view + owned per-frame logic over
// the master G block at 0x800E7E80.
//
// Verified this pass: **G IS TOMBA'S NODE**. He is not dispatched by walkAll / walkList2 /
// walkAux — his state lives on the shared G block that pool_init sub-init FUN_8007A810 zeros
// (0x184 bytes at 0x800E7E80..0x800E8004). Per-frame he ticks via the per-area callback
// `Engine::modePerFrameDispatch` fires (seaside area 0 = `area_seaside_perframe`, which invokes
// `ActorTomba::interactWalk`).
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::actorTomba`. Back-pointer wired
// once by Core's constructor. Callers reach Tomba's methods through the object graph:
//
//     c->engine.actorTomba.interactWalk();          // per-frame aux-item interaction loop
//     c->engine.actorTomba.growthStep(mode);        // grow (1) / shrink (0) transformation
//     c->engine.actorTomba.velocityIntegrate(suppressY);  // move via dir*speed at G+0x44/48/4A/4C
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// SceneTransition, Sop, ObjectList, ParallaxBg, ScreenFade.
//
// The methods below preserve their pre-class doc-comments — see the .cpp for the full RE.
#pragma once
#include <cstdint>
class Core;

class ActorTomba {
public:
  Core* core = nullptr;

  // Guest address of Tomba's node = the master G block.
  static constexpr uint32_t G_ADDR = 0x800E7E80u;

  // ----------------------------------------------------------------------------
  // Per-frame
  // ----------------------------------------------------------------------------
  // interactWalk — guest FUN_80022760. Walks the aux render list at *0x1F800154 and dispatches
  //   per-item collision checks against G. Early-outs on the "Tomba disabled" gates
  //   (G+0x16E == 0 / 0x800BF80D != 0 / G+0x17E & 0x200). Type-keyed dispatch to
  //   `proximityCheck` (types 0/1/2/3/7), `type4GuardedCheck` (type 4 subtype 2),
  //   `subHitboxCheck` (type 6).
  void interactWalk();

  // postInteractWalk — guest FUN_801130C4. Runs immediately after `interactWalk` (default-mode
  //   branch only). Walks a DIFFERENT list — the *0x1F80013C / *0x1F800144 pair — and per item
  //   dispatches by type:
  //     item[0xC] == 9 (special) → FUN_80111304(G, item) IF !(G+0x17E & 0x8200), continue
  //     item[2] == 3             → FUN_8010E258(G, item)
  //     item[2] == 4             → detailed guarded state-transition (see cpp)
  //     item[2] == 7             → FUN_800235A0(G, item)
  //     item[2] == 8             → FUN_800205CC(G, item)
  //     item[2] in {0xF, 0x14, 0x56} → FUN_80020364(G, item, 0)
  //     item[2] == 0x13          → FUN_8010EA80(G, item)
  //     item[2] == 0x2F          → FUN_80020364(G, item, 2)
  //   Sub-handler leaves stay substrate. Ghidra decomp scratch/decomp/fun_801130c4.c.
  void postInteractWalk();

  // ----------------------------------------------------------------------------
  // Growth / shrink (Tomba's transformation state)
  // ----------------------------------------------------------------------------
  // growthStep(mode) — guest FUN_80057DC0. Toggles G+0x17E bit 0x8000 (grown flag) with a
  //   G+0x32 Y-position ±0x46 compensation so his feet stay on the ground; then rescales the
  //   bounds/physics fields at G+0xB8/BA/BC (Q12 world scale), G+0x80/82/84/86 (bounding box),
  //   G+0x62/64/66/68 (physics constants), and DAT_800E802A by `1/(mode+1)`. Called by
  //   Engine::gStateMutate cases 6 (grow, mode=1) and 7 (shrink, mode=0).
  void growthStep(int32_t mode);

  // ----------------------------------------------------------------------------
  // Movement
  // ----------------------------------------------------------------------------
  // velocityIntegrate(suppressY) — guest FUN_80056B48. Integrates a per-frame velocity into
  //   Tomba's 16.16 master position: posX += dirX*speed (G+0x2C += (s16 G+0x48)*(s16 G+0x44)),
  //   posZ similarly (G+0x34 / G+0x4C), and posY (G+0x30 / G+0x4A) unless `suppressY` is set.
  //   Tail: if flag363 (G+0x16B) == 0 AND flag97 (G+0x61) == 0, dispatch the stop/settle helper
  //   FUN_80054650(G, 0); otherwise clear flag95 (G+0x5F) bit 0x04.
  void velocityIntegrate(bool suppressY);

  // settleStep(mode) — guest FUN_80054650. The "stop/settle helper" `velocityIntegrate` tail-
  //   dispatches when Tomba isn't blocked (flag363 & flag97 clear). Sets DAT_1F800258 = 0, clears
  //   G+0x5F bit 0x04, and if G+0x16B (flag363) is 0 runs a two-way grid probe (FUN_8004954C —
  //   substrate) with a `sVar3 = probe_offset / 2` derived from G+0x62 or from the item hooked
  //   at G+0x10 (obj+0x86 - obj+0x84 - (G+0x32 - obj+0x32)). On probe hit stamps G+0x60=1 and
  //   G+0x5F = (flag from G+0x149 bit 0x4 → G+0x147 else G+0x149&1) + 4, returns 1. Sets DAT_
  //   1F800258 marks a "sink" fallback that flips G+0x5F to (5 - G+0x147). Returns 1 on hit else 0
  //   (via c->r[2]). Ghidra decomp scratch/decomp/footstep_hunt.c.
  uint32_t settleStep(int32_t mode);

  // postFrameWaterCheck() — guest FUN_8010E904 (final call in area_seaside_perframe).
  //   Water/sea gating: if seaside water mode 0x800BF816 is engaged, snap Tomba's Y (G+0x32) to
  //   the water surface (0x800BF812 minus G+0x62); otherwise, if not paused, run FUN_8010E408(G)
  //   (substrate). Also fires the area-exit trigger (0x1F800137 / 0x800BF80F / 0x800BF83A /
  //   0x800BF839 / 0x1F800236) when Tomba is off-map (G+0x32 < -0xE74 AND G+0x36 < 0x1451) in
  //   water-mode 2. NO SFX. Ghidra decomp scratch/decomp/tomba_postframe_10e904.c.
  void postFrameWaterCheck();

private:
  // Sub-handlers of interactWalk — kept private since the type-dispatch loop is the only caller.
  void proximityCheck    (uint32_t item);     // FUN_80022060
  void type4GuardedCheck (uint32_t item);     // FUN_80114E74
  void subHitboxCheck    (uint32_t item);     // FUN_80022190
};
