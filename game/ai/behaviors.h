// game/ai/behaviors.h — class Behaviors: the per-object / per-area behavior handler surface.
//
// Static guest-ABI members (definitions stay one-per-file in game/ai/). The address-keyed dispatch
// (BehaviorDispatch::kTable / Engine::modePerFrameDispatch) takes plain fn pointers; static members
// convert to `void(*)(Core*)` so the dispatch mechanics are untouched. The 59 beh_* free handlers
// are the (deferred) consolidation target for this class.
#pragma once
class Core;

class Behaviors {
public:
  // areaSeasidePerframe (FUN_80113C5C, A00 overlay): the seaside per-area per-frame handler —
  // Tomba's mode-gated pre-update, the fixed mid-update trio, the aux-list walk, and the fixed
  // post-update pair. a0-free (reads the master G block directly). See area_seaside_perframe.cpp.
  static void areaSeasidePerframe(Core* c);
};
