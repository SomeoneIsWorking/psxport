// game/audio/sfx.h — PC-native SOUND-FX TRIGGER dispatcher.
//
// PROPER OOP: one instance per Core, embedded on Engine, reached as `eng(c).sfx.method(args)`.
// Back-pointer wired at Core construction (same pattern as Spawn / SceneEvents / Cull).
//
// SCOPE: FUN_80074590 — the field-wide SFX firing primitive. Two id spaces, both indexed into
// small stride-8 tables of pitch/pan/voice parameters, then handed to the SPU voice-fire leaf
// FUN_80075E04 with the assembled register + stack args:
//
//   * id 0..111    : per-area SFX table at 0x800A4D18[id] (stride 8). Common path — most gameplay
//                    SFX (footsteps, hits, pickups, arm/disarm beeps). Runs through the pause
//                    gate (0x1F800137 != 0 → t0=1 marker) before the voice-fire.
//   * id 112..127  : jumptable at 0x80016C04 (16 entries). Twelve entries remap to a small id and
//                    call FUN_80074BF8 (music/track control leaf); ids 125/126 are silent error;
//                    id 127 calls FUN_80074EEC (menu/UI SFX). These sub-leaves stay substrate.
//   * id 128..224  : per-area indirection table at 0x800A4EF8[area] → per-area SFX table indexed
//                    by (id & 0x7F), stride 8. Same voice-fire tail as the 0..111 path but with
//                    the a3 "flag" byte set to 128 (marks the area-specific origin).
//   * id 225..255  : silent no-op (recomp's `sltiu v0, v0, 225` gate).
//
// The tail computes a pitch-bend by (entry.byte6 + pitchBend_arg) × *(0x800FB165) / 9, clamped to
// [0, 127]; that's the pitch value passed to the SPU voice. Pan (int8 arg) is added to the
// entry's byte4 as a stack arg. All computation follows the recomp verbatim (int16 trims,
// 33-bit signed-div-by-9 via 0x38E38E39 magic) — verified against disas 0x80074590..0x80074808.

#pragma once
#include <cstdint>
struct Core;

class Sfx {
public:
  Core* core = nullptr;

  // trigger(id, pan, pitchBend): FUN_80074590. Fire the SFX by id with optional pan and pitch bend.
  //   * id        : 0..255 — dispatches per the id-space split above.
  //   * pan       : int8 — added to the SFX entry's byte4 for the SPU's stack arg (sp+16).
  //   * pitchBend : int8 — added to the SFX entry's byte6 before the /9 pitch computation.
  // Returns nothing; the recomp writes only to the SPU state through FUN_80075E04. The three
  // substrate leaves reachable from this method (FUN_80074BF8, FUN_80074EEC, FUN_80075E04) all
  // stay reachable by address via rec_dispatch — this method owns the CONTROL FLOW + the id-space
  // routing + the pitch/pan math, not the voice hardware.
  void trigger(int id, int pan, int pitchBend);

  // triggerPanned(id, pan): FUN_80074810 — the two-arg SFX entry. A thin wrapper that fires an
  // SFX by id with a pan offset and NO pitch bend (pitchBend forced to 0), then tail-calls the
  // firing primitive FUN_80074590. id is masked to a byte, pan is sign-extended from its low byte.
  // Mirrors the recomp's 24-byte frame (ra spilled at sp+16) so FUN_80074590 lands on the right
  // stack. Reachable both directly (shard_4 func_80074810) and via rec_dispatch from overlays.
  void triggerPanned(int id, int pan);

  // registerOverrides(): wires triggerPanned (0x80074810) into the global override registry so
  // every caller — direct func_80074810 and rec_dispatch — reaches the native method. Called once
  // from boot.cpp's register_engine_overrides.
  void registerOverrides();
};
