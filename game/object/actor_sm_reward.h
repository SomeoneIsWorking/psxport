// game/object/actor_sm_reward.h — PC-native "reward/tally window" actor SM family.
//
// FUN_80049A60 / FUN_80049E54 / FUN_8004A3D4 / FUN_8004B150 / FUN_8004B208 are five contiguous
// per-object state-machine STEPS (all take the generic object node in a0, a control/side byte in a1)
// called by the still-substrate FUN_8004AAC4 (an actor "process" dispatcher, itself unowned — outside
// this cluster's scope). Behavior observed from the RE (see actor_sm_reward.cpp for the full trace):
//   - FUN_80049A60 — a scroll/fade sub-SM: states 0 (init color+scroll-rate), 1 (scroll toward 0,
//     probing the grid to detect ground), 2 (post-scroll hold / re-probe / camera-lock branch),
//     3 (re-trigger scroll-out), 6 (despawn check via the cull wrapper). Writes a 24.8 fixed-point
//     accumulator (+0x30) and a blink bit (+0xd) shared with the sibling functions below.
//   - FUN_80049E54 — a numeric TALLY counter: ticks a displayed value (0x800E7FEE) toward a target by
//     a step, clamped to a cap (0x800BF87D), with a short "settle" countdown (+0x40) before reporting
//     done (v0==1).
//   - FUN_8004A3D4 — an EVENT dispatcher: given an event id (obj+0x68), gives items / sets inventory
//     flags / queues announcer cues, keyed by a large id table. Fully mechanical translation of the
//     RE'd switch; the id constants are NOT independently understood (only their GUEST EFFECTS, which
//     is what byte-exactness requires) — do not read named meaning into them.
//   - FUN_8004B150 / FUN_8004B208 — trivial init-once + shared blink-bit gates (8004B208 additionally
//     kicks a directional grid snap via FUN_80041194 on first entry).
//
// WIRING: the sole caller (FUN_8004AAC4) is SUBSTRATE, which calls each of these by a DIRECT C call
// (`func_<addr>(c)`, emitted by the recompiler) — that path checks the recompiler's OWN g_override[]
// table, not rec_dispatch. So registerOverrides() below installs each address into the single
// override registry (overrides::install) WITH a shard_set_override setter: the setter redirects the
// substrate's direct call, while the same registry entry also serves any native caller reaching
// these via rec_dispatch(c, addr) — both paths land here and get traced by the `dispatch` channel.
#pragma once
struct Core;
class  Game;

class ActorReward {
public:
  static void smWindowScroll(Core* c);   // FUN_80049A60(obj a0, side a1)
  static void smTallyTick(Core* c);      // FUN_80049E54(obj a0, step a1) -> v0
  static void smEventDispatch(Core* c);  // FUN_8004A3D4(obj a0) -> v0
  static void smBlinkA(Core* c);         // FUN_8004B150(obj a0, side a1)
  static void smBlinkB(Core* c);         // FUN_8004B208(obj a0, side a1)

  // --- WIDE-RE DRAFT (2026-07-08, region 0x80070000-0x8007FFFF) --- UNWIRED, UNVERIFIED. ---
  // FUN_80070018 is the reward/score-gem actor's TOP-LEVEL per-frame update: it drives the same
  // obj+4 state machine that ends up calling the FIVE methods above (smTallyTick/smEventDispatch/
  // Spawn::dropScoreGem), plus the position solver/approach helpers below and either
  // GraphicsBind::renderUpdateBody or the Spawn/Trig cull wrapper (FN_77B5C) depending on obj+0x5f
  // bit 0x80. Not independently wired: no override registration, no SBS run. See actor_sm_reward.cpp
  // for the field map and docs/engine_re.md for the region survey.
  static void update(Core* c);           // FUN_80070018(obj a0)
  static void resolvePosition(Core* c);  // FUN_800702C0(obj a0) -- position-source switch (obj+0x5e)
  static void approachTargetX(Core* c);  // FUN_80070650(obj a0) -- ease obj+0x2e toward obj+0x60

  // Wire all five guest addresses into the override registry (overrides::install), each with a
  // shard_set_override setter so the substrate's direct func_<addr>(c) calls from FUN_8004AAC4
  // redirect here too — a native caller reaching these via rec_dispatch also lands here and gets
  // `dispatch`-channel traced.
  static void registerOverrides(Game* game);
};
