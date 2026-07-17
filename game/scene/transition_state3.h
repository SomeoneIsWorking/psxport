// class TransitionState3 — the PC-native MID-TRANSITION state-3 entity walker.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::transitionState3`. Back-pointer
// `core` is wired once by Core's constructor. Callers reach the walker through the object graph:
//
//     eng(c).transitionState3.walkOnce();
//
// No `extern "C"` shim, no static method, no Core-as-first-arg. Same shape as SceneTransition,
// NodeXform, ScreenFade.
//
// Owns guest FUN_8007B04C (decomp scratch/decomp/ram_f1000_all.c L56987-L57017): the state-3
// entity walker called by `ov_field_frame_x` (the sm[0x4a]==5 mid-transition frame variant).
// Walks the two entity lists (heads T2_OBJLIST_HEAD_1 then T2_OBJLIST_HEAD_2, re-read after
// list 1 in case a handler mutated it), clears each node's per-frame render flag, and — gated
// on `node[0x28] & 0x80` — dispatches each object's handler via dispatch_obj_method (native beh
// if owned, substrate otherwise). Owning this here routes every mid-transition beh dispatch
// through dispatch_native_behavior, so the sub-scene swap state machine
// (`SceneTransition::stepSwapWaiter`) runs under native code during the transition.
#pragma once
#include <cstdint>
class Core;

class TransitionState3 {
public:
  // Back-pointer wired once by Core's constructor (same pattern as ScreenFade::core).
  Core* core = nullptr;

  // walkOnce: one call of the state-3 walker (guest FUN_8007B04C).
  void walkOnce();
};
