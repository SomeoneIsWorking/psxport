// class BgSceneTransitionSm — PC-native SOP intro BG scene-transition / screen-fade machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::bgSceneTransitionSm`. Back-pointer
// wired once by Core's constructor. Callers reach it through the object graph:
//
//     c->engine.bgSceneTransitionSm.step();
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as Demo,
// Sop, SceneTransition.
//
// Owns guest FUN_8002655C — the intro-cutscene fade manager (white→normal fade-in, hold, fade-to-black
// on scene/area change), driven by the scene-transition request code DAT_1F800236 and the
// direction/abort byte 0x800BF80F. Full RE + state map in bg_scene_transition_sm.cpp.
#pragma once
#include <cstdint>
class Core;

class BgSceneTransitionSm {
public:
  Core* core = nullptr;

  // step: one call = one frame of the transition machine (state 0 init/select, 1 fade-in, 2 hold,
  // 3 fade-out, 4 commit/restart). Ships with the `bgscenesmverify` byte-A/B gate.
  void step();
};
