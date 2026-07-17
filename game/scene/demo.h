// class Demo — the PC-native DEMO / front-end MENU stage state machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::demo`. Back-pointer wired once by
// Core's constructor. Callers reach the demo stage through the object graph:
//
//     eng(c).demo.stageMain();   // one-time entry (prologue + s0)
//     eng(c).demo.frame();       // one per-frame call (sm[0x48] substate dispatch + tail)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// ObjectList, SceneTransition, TransitionState3.
//
// Owns the DEMO overlay's SUBSTATE MACHINE (which substate runs, the sm[0x48] transitions and
// their field writes). The per-substate SYSTEM work (menu input machines, loaders, SFX, render)
// stays dispatched to the retained PSX code. Full RE map: docs/engine_re.md "DEMO / front-end
// MENU stage". Implementations + full doc-comments live in demo.cpp.
#pragma once
#include <cstdint>
class Core;
class Game;

class Demo {
public:
  Core* core = nullptr;

  // Wire s3SubMachine (0x80106AC4) into the override registry (overrides::install) — called from
  // runtime/recomp/boot.cpp's register_engine_overrides(). §9-verified 2026-07-10 (docs/
  // fleet-workflow.md §9): re-checked instruction-by-instruction against generated/ov_demo_shard_0.c;
  // only defect found was the missing guest-stack-frame mirror (now fixed in demo.cpp).
  static void registerOverrides(Game* game);

  // Live-spine entry points (called by the scheduler each frame — runtime/recomp/scheduler.cpp).
  void stageMain();      // one-time prologue + s0 (formerly ov_demo_stage_main)
  void frame();          // one per-frame substate dispatch + tail (formerly ov_demo_frame)

  // pc_faithful DEMO stage body — the whole ov_demo_gen_801062E4 arc as a native task body on a
  // PcScheduler fiber (faithful-execution model): prologue with guest frame -48 and live spills,
  // then the substate loop ending each iteration in the substrate tail (frame counter +
  // FUN_80051F80 yield). s0's texgroup spawn goes through rec_dispatch(0x80044BD4) -> the
  // registered spawn-and-wait override; s5's stage swap through rec_dispatch(0x80052078) (parks the
  // fiber; the stanza tears it down on the entry rewrite). pc_skip keeps stageMain()/frame().
  void stageBodyFaithful();

  // Substate handlers (called by frame() based on the current sm[0x48] substate).
  void s0();             // formerly ov_demo_s0

  // Slip #4 — DEMO s0 preload-wait step-spread (mirrors substrate FUN_80044BD4 spawn+yield rhythm).
  // s0PreYield: sm setup + loader + spawn task-1 (FUN_80044F58 preload); yields caller.
  // s0PostYield: run s0's post-FUN_80044BD4 tail (FUN_8007982C + reset75240 + FUN_8001CF00) and
  // advance sm[0x48] = 1. Called by run_demo_stanza's step-spread branch, not by frame() dispatch.
  void s0PreYield();
  void s0PostYield();
  // frame() case-0 fork. s0Skip (pc_skip): task-1 doesn't run via fiber under normal PC play, so
  // the preload runs synchronously inline (s0PreYield + preloadTexgroup + done_flag + s0PostYield).
  // s0Faithful currently shares that same body: the intended pre-yield/post-yield task-1 split is
  // a pending SBS-verified logic change (the demo_s0_step machinery is not advanced yet), so both
  // modes must stay byte-identical for now.
  void s0Skip();
  void s0Faithful();
  void s1();             // formerly ov_demo_s1
  void s2();             // formerly ov_demo_s2
  void s3();             // formerly ov_demo_s3
  void s6();             // formerly ov_demo_s6
  void s7Phase();        // formerly ov_demo_s7_phase (a sub-phase of the s7 dispatch)

  // DRAFT (UNWIRED, wide-RE fleet wave) — native port of the main-menu title cursor sub-machine
  // 0x80106AC4 that s3()/demo_frame_s3() still rec_dispatch. Returns v0 semantics identical to the
  // guest function (0/1/2/3); does not itself write sm[0x48] (callers own the transition). See
  // demo.cpp for the full RE + confidence notes. Not called from s3()/demo_frame_s3() yet — those
  // keep rec_dispatch(c, 0x80106ac4u) until this is SBS-verified.
  uint32_t s3SubMachine();   // FUN_80106AC4

  // Native port of the TITLE main-menu cursor sub-machine 0x8010696C that s2() rec_dispatches — the
  // s3SubMachine twin, minus the Circle/back outcome (the title menu can't cancel). Returns v0 (0/1/2);
  // does not write sm[0x48] (s2() owns the transition). Byte-exact frame (32; r31/r17/r16 @ sp+24/20/16).
  uint32_t s2SubMachine();   // FUN_8010696C

private:
  void s0Body();         // shared s0 case-0 body (see s0Skip/s0Faithful above)
};
