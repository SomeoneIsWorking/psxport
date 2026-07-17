// game/object/cube_text_ledger.h — RE'd leaves that back the "cube-text" popup family
// (game/ai/beh_cube_text_spawn.cpp, FUN_8003AD48) and its two callers already referenced but NOT
// independently understood from game/object/actor_sm_reward.cpp (FN_40B48/FN_40C00 there:
// "UI/event side-effect (leaf, not independently RE'd)"). This file supplies that RE + a faithful
// native port.
//
// WIRED 2026-07-08 (frontier pass): activateSlot (FN_40B48) and deactivateSlot (FN_40C00) have
// BOTH substrate callers (direct `func_<addr>(c)` from several shards) and a native caller
// (actor_sm_reward.cpp's ActorReward::smEventDispatch, via rec_dispatch) — installed into the
// override registry with a shard_set_override setter, same pattern as ActorReward, so both call
// paths reach the native body. spawnPopup (FN_40AA4) has only a substrate caller today but is
// installed with a setter anyway for future-proofing/tracing consistency, matching ActorReward's
// precedent.
// FN_40A58 (the size-class/cost table lookup) has NO owner of its own in this file: it is the
// SAME leaf as game/scene/scene_events.cpp's `SceneEvents::classSize` (identical two-level table
// walk over the same STR_TABLE_BASE/COST_TABLE addresses — see scene_events.h). This file used to
// carry a redundant copy (`CubeTextLedger::lookupCost`, deduped 2026-07-08 — dual-ownership found
// via codemap); activateSlot/deactivateSlot now call `eng(c).sceneEvents.classSize(slot, mode)`
// directly (mode 0 == nibbleLo=false / "start" cost, mode 1 == nibbleLo=true / "stop" cost). No
// dispatch wiring needed for it here either way — see registerOverrides().
//
// FOUR functions, one shared subsystem: a small fixed ledger of "popup slots" keyed by the SAME
// STRING TABLE that drives beh_cube_text_spawn's node[0x60] index (0x800A33C8, stride 12 bytes/
// entry — see beh_cube_text_spawn.cpp's tbl_strp()). Each entry's byte at +1 packs two nibbles
// (hi = "start" cost index, lo = "stop" cost index) into a 16-entry table of 32-bit "cost" values
// at 0x800A3B38. Activating/deactivating a slot adds that slot's cost into a running accumulator
// and appends a (slot, event) pair to a small ring log that game/scene/bg_scene_transition_sm.cpp
// ALREADY reads (0x800BF849 popup-active-count, 0x800ED06D log index) as its "drain before
// advancing" gate — so this ledger's consumers are already native; only these 4 producers/leaves
// were unowned.
//
// RE SOURCE: generated/shard_2.c:4542 (FUN_80040A58), shard_4.c:4944 (FUN_80040B48),
// shard_5.c:5496 (FUN_80040C00), shard_3.c:11258 (FUN_80040AA4) — the recompiler's INSTRUCTION-
// EXACT emission, walked register-by-register (ground truth per CLAUDE.md); Ghidra's decompile
// (scratch/decomp/region4004x.c, region4004x_b.c) used for structure/naming, cross-checked against
// the raw emission for every address computed from a `<hi>16<<16 + <lo>` pair.
//
// LAYOUT (base 0x800BF870 — the recompiler computes this base then indexes by fixed offsets;
// Ghidra's DAT_* names for the same cells are noted alongside):
//   +0x04 (u32)         RUNNING_COST         — accumulator; += a slot's cost table value on both
//                                              activate (start-index) and deactivate (stop-index).
//                                              (DAT_800BF874 — same cell game/world/spawn.cpp's
//                                              Spawn::dropScoreGem documents as "running AP total";
//                                              shared-address reuse, NOT independently reconciled
//                                              here — flagged for a future cross-check.)
//   +0x38 (u16)         ACTIVE_COUNT         — DAT_800BF8A8. ++ on a successful activate, and ALSO
//                                              (lazily) on a deactivate call that finds the slot
//                                              was never activated (flag byte == 0) — ground truth
//                                              of FUN_80040C00, reproduced as-is below.
//   +0x3A (u16)         DEACTIVATE_COUNT     — DAT_800BF8AA. ++ only on a REAL deactivate.
//   +0x44 (u8[]) SLOT_STATE array, indexed by slot — DAT_800BF8B4[slot]. 0 = inactive,
//                                              1 = active, 0xFF = deactivated (terminal; a second
//                                              deactivate on the same slot returns 0, no-op).
// Absolute addresses used directly below (base+offset already folded in, matching the recompiler's
// own constant folding at each call site):
//   G_LEDGER_GATE  0x800E7FEE (s16) — same cell as actor_sm_reward.cpp's G_TALLY_CUR ("tally
//                                     CURRENT/target display value"); read here only as a !=0 gate
//                                     ("some tally is in flight"), never written by this ledger.
//   LOG_INDEX      0x800ED06D (u8)  — ring-buffer write cursor, ++ on every activate/deactivate.
//   LOG_SLOT[]     0x800ED06E (u8)  — LOG_SLOT[LOG_INDEX] = slot, at the time of the event.
//   LOG_EVENT[]    0x800ED074 (u8)  — LOG_EVENT[LOG_INDEX] = 0 (activate) / 1 (deactivate).
//   STR_TABLE_BASE 0x800A33C8, stride 12 — the cube-text string table (beh_cube_text_spawn.cpp).
//     entry+1 (u8)  — packed nibbles: hi = start-cost index (mode 0), lo = stop-cost index (mode 1).
//   COST_TABLE     0x800A3B38, stride 4 (u32), 16 entries — indexed by the nibble above.
//   POPUP_ACTIVE_COUNT 0x800BF849 (u8) — ++ on FUN_80040AA4 spawn; -- on beh_cube_text_spawn's
//                                        STATE 2 despawn (already ported, game/ai/beh_cube_text_
//                                        spawn.cpp). This is the SAME counter bg_scene_transition_sm
//                                        waits to reach 0.
#pragma once
struct Core;
class  Game;

class CubeTextLedger {
public:
  // FUN_80040B48 (activate / scene-event ARM) is NOT owned here — it is SceneEvents::armBody
  // (game/scene/scene_events.cpp), wired by SceneEvents::registerOverrides. An earlier duplicate copy
  // (CubeTextLedger::activateSlot) was deduped onto that single owner (found via `codemap.py
  // --conflicts`; same treatment FUN_80040A58 got when it was folded onto SceneEvents::classSize).

  // FUN_80040C00(slot) -> v0. a0 = slot.
  //   v0 = -1  if G_LEDGER_GATE == 0
  //   v0 =  0  if SLOT_STATE[slot] == 0xFF (already deactivated — terminal, no-op)
  //   v0 =  1  otherwise: (if SLOT_STATE[slot]==0, ACTIVE_COUNT++ anyway — ground-truth quirk,
  //            reproduced exactly); SLOT_STATE[slot]=0xFF; DEACTIVATE_COUNT++;
  //            RUNNING_COST += SceneEvents::classSize(slot, /*nibbleLo=*/true) (low nibble = "stop"
  //            cost index); append (slot, event=1); LOG_INDEX++.
  static void deactivateSlot(Core* c); // a0 = slot; sets v0 (r2)

  // FUN_80040AA4(value, variant) -> v0 (node ptr, or 0 on freelist exhaustion). a0=value, a1=variant.
  // Allocates a node via the (still-unowned, opaque) freelist call FUN_8007A980(4,3,1), then:
  //   node[0x1C] = 0x8003AD48        (vtable = beh_cube_text_spawn)
  //   node[0x02] = 0x0B              (object TYPE tag)
  //   node[0x03] = variant
  //   node[0x60] = value             (u16 — the cube-text STRING TABLE index; beh_cube_text_spawn
  //                                   reads this back via its tbl_strp() helper)
  //   node[0x28] |= 0x80             (active flag bit — the generic per-object pattern)
  //   POPUP_ACTIVE_COUNT (0x800BF849)++
  //   FUN_800727D4(node, value, variant)   (still-unowned init call — invoked, not reimplemented)
  // Returns the node pointer (0 if the allocator returned null).
  static void spawnPopup(Core* c);     // a0 = value, a1 = variant; sets v0 (r2)

  // Wire activateSlot/deactivateSlot/spawnPopup into the override registry (overrides::install),
  // each with a shard_set_override setter so the substrate's direct func_<addr>(c) calls redirect
  // here too, in addition to ActorReward's rec_dispatch(c, FN_40B48/FN_40C00) calls landing here.
  // lookupCost is deliberately NOT registered — see the file header "WIRED 2026-07-08" note.
  static void registerOverrides(Game* game);
};
