// class Demo — the PC-native DEMO / front-end MENU stage state machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::demo`. Back-pointer wired once by
// Core's constructor. Callers reach the demo stage through the object graph:
//
//     c->engine.demo.stageMain();   // one-time entry (prologue + s0)
//     c->engine.demo.frame();       // one per-frame call (sm[0x48] substate dispatch + tail)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// ObjectList, SceneTransition, TransitionState3.
//
// Owns the DEMO overlay's SUBSTATE MACHINE (which substate runs, the sm[0x48] transitions and
// their field writes). The per-substate SYSTEM work (menu input machines, loaders, SFX, render)
// stays dispatched to the retained PSX code. Full RE map: docs/engine_re.md "DEMO / front-end
// MENU stage". Implementations + full doc-comments live in engine_demo.cpp.
#pragma once
#include <cstdint>
class Core;

class Demo {
public:
  Core* core = nullptr;

  // Live-spine entry points (called by the scheduler each frame — runtime/recomp/scheduler.cpp).
  void stageMain();      // one-time prologue + s0 (formerly ov_demo_stage_main)
  void frame();          // one per-frame substate dispatch + tail (formerly ov_demo_frame)

  // Substate handlers (called by frame() based on the current sm[0x48] substate).
  void s0();             // formerly ov_demo_s0
  void s1();             // formerly ov_demo_s1
  void s2();             // formerly ov_demo_s2
  void s3();             // formerly ov_demo_s3
  void s6();             // formerly ov_demo_s6
  void s7Phase();        // formerly ov_demo_s7_phase (a sub-phase of the s7 dispatch)
};
