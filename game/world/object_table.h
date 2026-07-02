// class ObjectTable — the PC-native 40-slot FIXED OBJECT TABLE dispatcher.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::objectTable`. Back-pointer wired
// once by Core's constructor. Callers reach the dispatcher through the object graph:
//
//     c->engine.objectTable.dispatch();
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// ObjectList / Array8Dispatch / TransitionState3 / SceneTransition.
//
// Owns guest FUN_80026C88: iterates the 40-slot fixed OBJECT TABLE at 0x800EC188 (stride 64);
// for each ACTIVE slot (byte[0] != 0), dispatches its handler via the fn-ptr table at
// 0x800AD52C indexed by byte[1] (a0 = slot). Faithful to the recomp body — the callee handlers
// stay substrate (each honors its own owned override in the super-call path).
//
// Ships with the `disp26c88verify` A/B gate: on that channel, every dispatch is compared byte-
// for-byte against rec_super_call(0x80026C88) with a scoped exclusion for the dead stack region
// below entry sp (per the pre-restructure verify's stack-cleanup notes).
#pragma once
#include <cstdint>
class Core;

class ObjectTable {
public:
  Core* core = nullptr;

  static constexpr uint32_t TABLE_BASE     = 0x800EC188u;  // slot 0 base
  static constexpr uint32_t SLOT_STRIDE    = 64u;          // 40 × 64B slots
  static constexpr int      SLOT_COUNT     = 40;
  static constexpr uint32_t HANDLER_TABLE  = 0x800AD52Cu;  // byte[1] -> u32 handler

  // dispatch: one call = one sweep of the 40-slot table (was ov_disp_26c88 / FUN_80026C88).
  void dispatch();
};
