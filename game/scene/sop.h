// class Sop — the PC-native SOP (intro-cutscene) FIELD stage machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::sop`. Back-pointer wired once by
// Core's constructor. Callers reach the SOP stage through the object graph:
//
//     c->engine.sop.fieldMode();     // per-frame outer state dispatcher (was ov_sop_field_mode)
//     c->engine.sop.fieldUpdate();   // per-frame gameplay body        (was ov_sop_field_update)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as Demo,
// ObjectList, SceneTransition, TransitionState3.
//
// Owns the SOP overlay's FIELD-MODE state machine (guest FUN_80109450) and the per-frame FIELD
// update body (guest FUN_801092B4). Implementations + full doc-comments live in sop.cpp.
#pragma once
#include <cstdint>
class Core;

class Sop {
public:
  Core* core = nullptr;

  // Live-spine entry points.
  void fieldMode();     // was ov_sop_field_mode  (per-frame outer state dispatcher)
  void fieldUpdate();   // was ov_sop_field_update (per-frame gameplay body — states 1/2/3)
};
