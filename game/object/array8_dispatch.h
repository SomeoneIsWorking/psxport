// class Array8Dispatch — the PC-native 8-slot FIXED OBJECT ARRAY dispatcher.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::array8Dispatch`. Back-pointer wired
// once by Core's constructor. Callers reach the dispatcher through the object graph:
//
//     eng(c).array8Dispatch.tick();
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// ObjectList, TransitionState3, SceneTransition.
//
// Owns guest FUN_80026368: iterates the 8-slot fixed object array at 0x80100400 (stride 0x4C); for
// each ACTIVE slot (byte[0] != 0) dispatches its method by type byte[2] through the jump table at
// 0x8009D314 (a0 = slot). Faithful to the recomp body: no type bound-check (guest indexes raw),
// inactive slots still advance.
#pragma once
#include <cstdint>
class Core;

class Array8Dispatch {
public:
  Core* core = nullptr;

  // Guest constants for the fixed array + type dispatch table.
  static constexpr uint32_t ARRAY_BASE   = 0x80100400u;   // slot 0 base
  static constexpr uint32_t SLOT_STRIDE  = 0x4Cu;         // 76-byte slots, 8 total
  static constexpr uint32_t METHOD_TABLE = 0x8009D314u;   // type byte -> u32 handler

  // tick: one call = one full sweep of the 8-slot array. Was `ov_arr8_dispatch_26368` /
  // rec_dispatch(0x80026368).
  void tick();

  // tickFaithful(): byte-exact mirror of gen_func_80026368 -- reproduces the guest stack frame
  // (sp -= 32; spill s0/s1/s2/ra at sp+16/20/24/28) and the per-slot jal-site r31 (0x800263C0)
  // that the plain tick() loop does not. Used under pc_faithful (SBS core A / mPcSkip=false).
  void tickFaithful();
};
