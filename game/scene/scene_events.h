// game/scene/scene_events.h — PC-native SCENE-EVENT ARM subsystem, owned by Engine.
//
// PROPER OOP: one instance per Core, embedded on Engine, reached as
// `eng(c).sceneEvents.method(args)`. Back-pointer wired at Core construction.
//
// SCOPE: the field-wide EVENT ARM primitive FUN_80040B48 (idempotent per-slot arm) and its size-class
// helper FUN_80040A58 (two-level table lookup: table A @0x800A33C8, stride 12; table B @0x800A3B38,
// stride 4). The class OWNS these two entry points; other engine paths that need to consult the
// size-class table can call `classSize` directly (the recomp still has multiple substrate callers of
// FUN_80040A58 — the shard-emitted func_80040A58 remains reachable via its address).
//
// STATE (field-wide, kept in guest RAM — this class is stateless, just methods over Core*):
//   0x800E7FEE  int16   global "events enabled" gate (0 = disabled → arm() returns -1)
//   0x800BF870  base    per-slot arm-flag table (byte at +arg+68 is the "already armed" flag)
//   0x800BF8A8  u16     global arm counter (bumped on every fresh arm)
//   0x800BF874  u32     event-stream write cursor (advances by classSize on every fresh arm)
//   0x800ED058  base    event ring records (stride implied by the FUN_80040A58 size class)
//   0x800ED06D  u8      ring write-head byte (record[idx]+0x16 = arg, +0x1C = 0, then idx++)
#pragma once
#include <cstdint>
struct Core;

class SceneEvents {
public:
  Core* core = nullptr;

  // arm(eventId): FUN_80040B48 — idempotent scene-event arm. Returns:
  //    1 on fresh arm (this call armed the slot; caller may treat as "consumed → advance"),
  //    0 if the slot was already armed (no-op — caller should wait/retry next frame),
  //   -1 if the global events gate at *(int16_t)0x800E7FEE is 0 (event system disabled).
  // Callers that ignore the return (most SFX / area-fade sites) can drop it on the floor.
  int32_t arm(uint8_t eventId);

  // classSize(argKey, nibbleLo): FUN_80040A58 — two-level table lookup returning the byte-size /
  //   stream stride for size class N (0..15). Reads table A @0x800A33C8 (stride 12) at
  //   [argKey*12 + 1]; picks the HIGH nibble when nibbleLo==false (FUN_80040B48's usage) or the LOW
  //   nibble when true; indexes table B @0x800A3B38 (stride 4) and returns the u32 there. Kept
  //   public so other subsystems that consult the same size class can share this path.
  uint32_t classSize(uint8_t argKey, bool nibbleLo);

  // FUN_80040B48 override thunk (guest ABI: slot in r4, ret in r2). SceneEvents is the SOLE owner of
  // FUN_80040B48 — registerOverrides installs it into the override registry (overrides::install),
  // with a shard_set_override setter so both rec_dispatch callers (e.g. ActorReward) and the
  // recompiler's g_override[] (substrate func_80040B48 sites) reach the same native body — the
  // registry's own oracle-leg gate keeps psx_fallback running the recompiled body.
  // cube_text_ledger.cpp's CubeTextLedger::activateSlot used to be an independent second copy of this
  // exact body (found via `codemap.py --conflicts`); it was deduped onto this owner, the same way
  // FUN_80040A58 was deduped onto classSize.
  static void armOverride(Core* c);

  // Scene-command record handlers (r4 = pointer to a command record; ret in r2). Both are leaves —
  // pure guest-state readers/writers, no frame, no sub-calls.
  //   delayedTrigger (FUN_80042258): two-phase dwell trigger. Phase 0 resets the dwell timer and
  //     advances to phase 1; phase 1 latches the record's args into the global param halves per the
  //     selector's low bits, then fires (returns 1) once the arm-ready byte is set OR the dwell timer
  //     reaches 500, else keeps waiting (returns 0). Returns 0 for any other phase.
  //   applyFlagOp   (FUN_80042448): set/OR/AND a byte in the flag table (base 0x800BF850, entry
  //     @ +argA+324) selected by the record's op mode (0=set, 1=OR, 2=AND); always returns 1.
  static void delayedTriggerOverride(Core* c);   // FUN_80042258
  static void applyFlagOpOverride(Core* c);      // FUN_80042448

  static void registerOverrides(class Game* game);

private:
  // Guest-ABI arm body (plain fn-pointer shape for the verify gate).
  static uint32_t armBody(Core* c);   // FUN_80040B48

  static uint32_t delayedTrigger(Core* c);   // FUN_80042258 body
  static uint32_t applyFlagOp(Core* c);      // FUN_80042448 body
};
