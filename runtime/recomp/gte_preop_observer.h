// gte_preop_observer.h — per-Core, explicitly armed observer at the guest GTE-op boundary.
//
// This is diagnostic plumbing, not render behaviour. An unarmed observer changes no counters and
// calls nothing. The callback runs BEFORE the GTE instruction, so it may read DR0..DR5 as the exact
// guest XYZ operands that the instruction is about to consume. `seen` is the denominator: every GTE
// op encountered while armed increments it, even if a game-side callback filters that op out.
#pragma once

#include <cstdint>

class Core;

using GtePreOpFn = void (*)(Core* core, uint64_t ordinal, uint32_t guestPc,
                            uint32_t instruction, void* user);

class GtePreOpObserver {
public:
  void arm(GtePreOpFn fn, void* user) {
    mFn = fn;
    mUser = user;
    mSeen = 0;
  }

  uint64_t disarm() {
    const uint64_t seen = mSeen;
    mFn = nullptr;
    mUser = nullptr;
    return seen;
  }

  bool armed() const { return mFn != nullptr; }
  uint64_t seen() const { return mSeen; }

  void observe(Core* core, uint32_t guestPc, uint32_t instruction) {
    if (!mFn) return;
    const uint64_t ordinal = ++mSeen;
    mFn(core, ordinal, guestPc, instruction, mUser);
  }

private:
  GtePreOpFn mFn = nullptr;
  void* mUser = nullptr;
  uint64_t mSeen = 0;
};
