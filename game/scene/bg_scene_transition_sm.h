// class BgSceneTransitionSm — PC-native SOP intro BG scene-transition / screen-fade machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::bgSceneTransitionSm`. Back-pointer
// wired once by Core's constructor. Callers reach it through the object graph:
//
//     eng(c).bgSceneTransitionSm.step();
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

  // readyForProgress (FUN_80042728): scene-progress predicate — true iff both scene-progress
  // counters at *(0x800BF849) and *(0x800ED06D) are zero. Used by beh_ handlers whose scene-trigger
  // sub-state advances only when nothing is holding the scene state pending (e.g. the hold branch of
  // bg_scene_transition_sm state 2 uses the same condition inline). Was rec_dispatch(0x80042728u).
  bool readyForProgress() const;

  // registerOverrides — install the native leaves this class owns into the one override registry.
  void registerOverrides();

  // opSceneEventArmWait (FUN_80042758): cutscene-script opcode leaf. Sub-state at node+120 drives a
  // two-step wait: state 0 arms a scene event via SceneEvents::arm(node+114) and, once armed and
  // node+116 has settled to 0, bumps the sub-state; state 1 polls the readyForProgress predicate
  // (FUN_80042728). Returns the opcode-progress result in v0. READY-FRAME (sp-24, spills s0/ra).
  static void opSceneEventArmWait(Core* c);

  // opClearSceneFlag80a (FUN_80042884): cutscene-script opcode leaf — clears the scene sub-state
  // flag byte at 0x800BF80A and returns 1.
  static void opClearSceneFlag80a(Core* c);

private:
  // Guest-ABI SM body + verify harness + the tiny native sub-leaves it calls (see .cpp for RE).
  static void body(Core* c);                            // FUN_8002655C
  static void verifyBody(Core* c);                      // bgscenesmverify A/B wrapper
  static void fadeRect(Core* c, uint32_t color);        // host-side fade delivery (same arg shape as
                                                        // the guest fade leaf; owner = ScreenFade tap)
  static void audioFadeTarget(Core* c, int32_t v);      // FUN_80075CEC
  static bool midTransitionGate(Core* c);               // shared 26470/26510/264BC guard
  static void audioStub26470(Core* c);                  // FUN_80026470
  static void audioStub26510(Core* c);                  // FUN_80026510
  static void audioStub264BC(Core* c);                  // FUN_800264BC
  static void bf816Dispatch(Core* c);                   // FUN_80050970
};
