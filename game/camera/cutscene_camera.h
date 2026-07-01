// class CutsceneCamera — the general FIELD / FOLLOW camera of Tomba! 2, PC-native, expressed as PC-game
// STRUCTURE (named state, methods) rather than a register-convention transcription.
//
// SCOPE (RESOLVED 2026-07-01, later-293 — docs/findings/camera.md): this IS the free-roam field camera, not
// a cutscene-only camera. The earlier "cutscene-only, free-roam lives in a 0x8013xxxx overlay" conclusion was
// a MEASUREMENT ARTIFACT: camtrace/recdep hook `rec_dispatch`, but the recompiler emits intra-MAIN calls as
// DIRECT C calls (`func_8006EC44(c)`), so resident→resident camera dispatch was invisible to those meters. A
// guest-stack backtrace at the view-matrix write proved the free-roam chain is the resident driver
// 0x8006EC44 → snapFollow (0x8006E3B0) → lookAt (0x8006D02C) — all owned here. The class name is now a
// misnomer (rename to `Camera` deferred, low priority); it serves SOP/cutscene AND free-roam.
//
// The per-frame DRIVER + its jump-table MODES + the init/mode-selector are `update()` / `init()` below.
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

  // The resident per-frame driver (0x8006EC44) hardcodes the camera OBJECT at this fixed base; `update()`
  // operates on it. (The SOP snapFollow wiring passes a per-scene base instead, which is fine — the object
  // layout is identical; only the driver hardcodes 0x800E8008.)
  static constexpr uint32_t CAM_OBJ = 0x800E8008u;

  CutsceneCamera(Core* c, uint32_t cam) : c(c), cam_(cam) {}

  // ── Per-frame DRIVER (0x8006EC44) + init/mode-selector (0x8006EA7C) ──────────────────────────────
  // update(): the resident camera driver — reads the outer state from cam[0] (0=first-frame init, 1=run,
  //   else idle), runs the sub-state machine, then dispatches on the MODE byte cam[0x64]&0x3F (18-entry
  //   jump table) to one of the follow orchestrators / a still-substrate leaf / a field-overlay handler,
  //   and finally runs the post-mode tail (0x8006C988). Owns the CALLER of the already-owned orchestrators
  //   (contiguous top-down ownership); still-unowned resident leaves + all field overlays run via the
  //   substrate. Reached indirectly (camera object behaviour pointer) — wire when the object walk is native.
  void update();
  void init();   // 0x8006EA7C — first-frame field reset + render-mode-keyed mode selector.
  void initPlace();               // FUN_8006E918 — init: place camera X/Z base (S+0x02/S+0x0a) from heading.
  void initSeedGrp(uint32_t src); // FUN_8006CBA8 — init: seed cam[0x3a/0x3e/0x42] from a source group.

  // ── Orchestrators (per-frame camera MODES). One is picked per frame by the caller/mode selector. ──
  void snapFollow(uint32_t target);    // FUN_8006E3B0 — SOP/cutscene: SNAP the follow accumulators to
                                       //   the target (no smoothing), then build the view (lookAt).
  void mainFollow();                   // FUN_8006E0F0 — the smoothing MAIN follow (dist→track→pitch→…).
  void simpleFollow(uint32_t target);  // FUN_8006E3F4 — track XZ then Y (settled bits), then lookAt.
  void trackFollow(uint32_t target);   // FUN_8006E228 — track + 2 substrate sub-fns, then lookAt.
  // Scripted-camera SNAP variants (driver modes 2/3/4). Each SNAPs the accumulators to the target, adds an
  // extra step, then lookAt. The scripted look-angle builders they use (0x8006DC38/DF88/DAD8/DEF0) stay
  // substrate for now (a cohesive future unit — rsin/rcos heading/pitch builders into the S block).
  void snapFollowA(uint32_t target);   // FUN_8006E294 (mode 2 + init post-check): snap + look-build A.
  void pitchFollow(uint32_t target);   // FUN_8006E360 (mode 3): pitch, then snap, then lookAt.
  void snapFollowB(uint32_t target);   // FUN_8006E2FC (mode 4): snap + look-build B.

  // ── Sub-ops (the follow pipeline steps; public so the per-call verify wrappers can drive them). ──
  bool trackXZ(uint32_t target);       // FUN_8006D960 — smooth camera X/Z toward target; ret settled.
  bool trackY(uint32_t target);        // FUN_8006DA54 — smooth camera Y toward target;   ret settled.
  void snapAccXZ(uint32_t target);     // FUN_8006D934 — SNAP the X/Z follow accumulators to the target.
  void snapAccY(uint32_t target);      // FUN_8006D950 — SNAP the Y follow accumulator to the target.
  // Scripted-camera LOOK-ANGLE builders (used by snapFollowA/B). posBuild* place the X/Z look accumulators
  // (S+0/S+8) from the camera's rcos/rsin angles (cam[0x6c/0x6e/0x70]); headBuild* step the heading
  // accumulator (S+6). Pair A (posBuildA+headBuildA) serves mode 2; pair B serves mode 4.
  void posBuildA();                    // FUN_8006DC38 — direct place (overwrite S+0/S+8).
  void posBuildB();                    // FUN_8006DAD8 — place then yaw/dist ACCUMULATE (shares lookat tail).
  void headBuildA(uint32_t nonzero);   // FUN_8006DF88 — heading step (a1==0: fixed offset; else: rsin step).
  void headBuildB();                   // FUN_8006DEF0 — heading step with the ±10 snap (like heading()).
  void distSolve();                    // FUN_8006D2AC — distance/zoom solver + planar placement.
  void pitch();                        // FUN_8006D654 — vertical-look height smoother.
  void yFloor();                       // FUN_8006C80C — per-render-mode camera-Y floor clamp.
  void heading();                      // FUN_8006DCF4 — heading tracker.
  void angleStep();                    // FUN_8006E010 — angle accumulator step.
  void rotBuild();                     // FUN_8006E464 — rotation/look-at builder (special-cam modes).
  void lookAt();                       // FUN_8006D02C — build the camera basis/view matrix into S.
  // Post-mode TAIL (0x8006C988) — the camera SHAKE state machine; update() runs it after every mode, every
  // frame. cam[0x76] is the shake state (driven by external code: 0=idle); cam[0x86/0x88/0x8a] hold the
  // pre-shake anchor snapshot of the look position (S+0x02/0x06/0x0a). See cutscene_camera.cpp for the
  // per-state breakdown (3-axis free-running shake, Y-only free-running shake, two Y-only one-shot pulses).
  void shakeTail();                    // FUN_8006C988

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
  int32_t call(uint32_t fn, int32_t a0 = 0, int32_t a1 = 0, int32_t a2 = 0, int32_t a3 = 0);
  // Dispatch a still-unowned resident camera LEAF (or a field overlay handler) via the substrate, mirroring
  // the driver's `jal fn` exactly: set only a0 (=cam_) and, when given, a1 — leave a2/a3 alone (verified the
  // targets don't read them). update()/init() use these for the leaves/overlays they don't own yet.
  void sub(uint32_t fn)              { c->r[4] = cam_; rec_dispatch(c, fn); }
  void sub(uint32_t fn, uint32_t a1) { c->r[4] = cam_; c->r[5] = a1; rec_dispatch(c, fn); }
  // update()'s MODE-byte (cam[0x64]&0x3F) dispatch — split out to keep update() readable.
  void dispatchMode(uint8_t mode);
  // Shared look-at tail (rotBuild's modes): place a look point around the scene centre at (theta,radius),
  // fold yaw+distance into the pitch accumulators.
  void lookatTail(int32_t theta, int32_t radius);
  // The yaw/dist accumulate tail shared by lookatTail and posBuildB: from a look-relative (dx,dz), turn the
  // heading toward it and fold cos·dist/2, sin·dist/2 into the S+0/S+8 accumulators; settle bit if dist<401.
  void yawDistAccumulate(int32_t dx, int32_t dz);
  void joinE640(int32_t delta, int32_t radius);
  int32_t table1Delta();
  void table2(int32_t radius);
};
