// class CutsceneCamera — the CUTSCENE / scripted-mode camera of Tomba! 2, PC-native, expressed as PC-game
// STRUCTURE (named state, methods) rather than a register-convention transcription.
//
// CUTSCENE vs INGAME (verified 2026-07-01, correcting the handoff — docs/findings/camera.md): the resident
// MAIN.EXE camera code this class owns is the SOP / CUTSCENE + special-mode camera. It is driven live by
// game/scene/sop.cpp (the intro-cutscene BG camera, `snapFollow`). The A00 **INGAME free-roam** field
// camera is a SEPARATE subsystem — it is NOT this code (measured: zero resident-camera dispatch across 200
// moving free-roam frames); it lives in the MODE-slot field overlay (0x8013xxxx cluster) and is a distinct
// future class (`class FieldCamera`). Do not conflate the two: this is the cutscene camera only.
//
// STATE MODEL. The camera keeps its working state in two places, both accessed guest-direct behind named
// accessors (no mandatory mirror, per docs/pc-subsystem-rebuild.md):
//   * the per-instance CAMERA OBJECT (`cam_`, a main-RAM struct; its base varies by scene — SOP uses
//     0x800E8008) — position, distance, velocities, mode flags, settled bits.
//   * the GLOBAL camera/scene state block `G` (0x800E7E80) — the master position (player+camera share it
//     in this 2.5D game), scene heading, mode selectors, and the SCRATCHPAD scratch block `S`
//     (0x1F8000D0) that holds the follow accumulators + the composed view matrix the projection reads.
// Method params replace the guest register convention (no `c->r[4]=cam`); the arithmetic is the RE'd
// engine behaviour, verified per-call against the recomp oracle (`camverify`) — see camera.cpp.
#pragma once
#include "core.h"
#include <stdint.h>

// Debug-teleport hook (REPL `tp X Y Z`): moves Tomba's MASTER position so the whole follow pipeline
// tracks it naturally. Kept as free functions — they are REPL glue, not camera behaviour.
void cam_teleport(int x, int y, int z);
void cam_teleport_off(void);
void engine_camera_register(void);   // wiring entry (called from game_tomba2 boot)

class CutsceneCamera {
public:
  // GLOBAL camera/scene state block (master position, heading, mode selectors). One per game.
  static constexpr uint32_t G = 0x800E7E80u;
  // SCRATCHPAD camera scratch block: follow accumulators + the composed view matrix. Named offsets from
  // S below. The projection + the still-substrate cull read S, so writes here are a content interface.
  static constexpr uint32_t S = 0x1F8000D0u;

  // Master world position (player and camera-center share it — 2.5D). 16.16 fixed, in G.
  static constexpr uint32_t MASTER_X = G + 0x2C;   // 0x800E7EAC
  static constexpr uint32_t MASTER_Y = G + 0x30;   // 0x800E7EB0
  static constexpr uint32_t MASTER_Z = G + 0x34;   // 0x800E7EB4

  CutsceneCamera(Core* c, uint32_t cam) : c(c), cam_(cam) {}

  // ── Orchestrators (per-frame camera MODES). One is picked per frame by the caller/mode selector. ──
  void snapFollow(uint32_t target);    // FUN_8006E3B0 — SOP/cutscene: SNAP the follow accumulators to
                                       //   the target (no smoothing), then build the view (lookAt).
  void mainFollow();                   // FUN_8006E0F0 — the smoothing MAIN follow (dist→track→pitch→…).
  void simpleFollow(uint32_t target);  // FUN_8006E3F4 — track XZ then Y (settled bits), then lookAt.
  void trackFollow(uint32_t target);   // FUN_8006E228 — track + 2 substrate sub-fns, then lookAt.

  // ── Sub-ops (the follow pipeline steps; public so the per-call verify wrappers can drive them). ──
  bool trackXZ(uint32_t target);       // FUN_8006D960 — smooth camera X/Z toward target; ret settled.
  bool trackY(uint32_t target);        // FUN_8006DA54 — smooth camera Y toward target;   ret settled.
  void distSolve();                    // FUN_8006D2AC — distance/zoom solver + planar placement.
  void pitch();                        // FUN_8006D654 — vertical-look height smoother.
  void yFloor();                       // FUN_8006C80C — per-render-mode camera-Y floor clamp.
  void heading();                      // FUN_8006DCF4 — heading tracker.
  void angleStep();                    // FUN_8006E010 — angle accumulator step.
  void rotBuild();                     // FUN_8006E464 — rotation/look-at builder (special-cam modes).
  void lookAt();                       // FUN_8006D02C — build the camera basis/view matrix into S.

  Core* c;
  uint32_t cam_;

private:
  // Short guest-memory accessors (keep the logic readable; guest-direct is intentional).
  uint8_t  r8 (uint32_t a) { return c->mem_r8 (a); }
  uint16_t r16(uint32_t a) { return c->mem_r16(a); }
  uint32_t r32(uint32_t a) { return c->mem_r32(a); }
  void     w8 (uint32_t a, uint8_t  v) { c->mem_w8 (a, v); }
  void     w16(uint32_t a, uint16_t v) { c->mem_w16(a, v); }
  void     w32(uint32_t a, uint32_t v) { c->mem_w32(a, v); }
  // CutsceneCamera-object field accessors (the base varies per scene, so field access is offset-from-cam_).
  uint8_t  camR8 (uint32_t off) { return c->mem_r8 (cam_ + off); }
  uint16_t camR16(uint32_t off) { return c->mem_r16(cam_ + off); }
  uint32_t camR32(uint32_t off) { return c->mem_r32(cam_ + off); }
  void     camW8 (uint32_t off, uint8_t  v) { c->mem_w8 (cam_ + off, v); }
  void     camW16(uint32_t off, uint16_t v) { c->mem_w16(cam_ + off, v); }
  void     camW32(uint32_t off, uint32_t v) { c->mem_w32(cam_ + off, v); }

  // One follow-accumulator axis (shared by trackXZ/trackY): snap when within ±10 of the target integer,
  // else rate-limited step. accAddr = 32-bit fixed accumulator; its high half is the integer the cull
  // reads. Returns true iff it SNAPPED (settled) this frame.
  bool followAxis(uint32_t accAddr, uint32_t tgt32Addr, uint16_t tgtInt, uint16_t curInt, int16_t maxStep);
  // Call a retained libgte trig/matrix helper (rsin/rcos/ratan2/isqrt/…) via the substrate — they are a
  // math library, not PSX hardware, so they stay substrate. Returns v0.
  int32_t call(uint32_t fn, int32_t a0, int32_t a1 = 0, int32_t a2 = 0);
  // Shared look-at tail (rotBuild's modes): place a look point around the scene centre at (theta,radius),
  // fold yaw+distance into the pitch accumulators.
  void lookatTail(int32_t theta, int32_t radius);
  void joinE640(int32_t delta, int32_t radius);
  int32_t table1Delta();
  void table2(int32_t radius);
};
