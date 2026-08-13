// gte_preop_observer.h — per-Core, explicitly armed observer at the guest GTE-op boundary.
//
// This is diagnostic plumbing, not render behaviour. An unarmed observer changes no counters and
// calls nothing. The pre callback runs BEFORE the GTE instruction, so it may read DR0..DR5 as the
// exact guest XYZ operands that the instruction is about to consume; the optional post callback runs
// immediately AFTER it with the same PC, instruction and ordinal. `seen` is the denominator: every
// GTE op encountered while either callback is armed increments it, even if a callback filters it out.
#pragma once

#include <cstdint>

class Core;

using GtePreOpFn = void (*)(Core* core, uint64_t ordinal, uint32_t guestPc,
                            uint32_t instruction, void* user);
using GtePostOpFn = void (*)(Core* core, uint64_t ordinal, uint32_t guestPc,
                             uint32_t instruction, void* user);

class GtePreOpObserver {
public:
  void arm(GtePreOpFn fn, void* user) {
    arm(fn, nullptr, user);
  }

  void arm(GtePreOpFn preFn, GtePostOpFn postFn, void* user) {
    mPreFn = preFn;
    mPostFn = postFn;
    mUser = user;
    mSeen = 0;
  }

  uint64_t disarm() {
    const uint64_t seen = mSeen;
    mPreFn = nullptr;
    mPostFn = nullptr;
    mUser = nullptr;
    return seen;
  }

  bool armed() const { return mPreFn != nullptr || mPostFn != nullptr; }
  uint64_t seen() const { return mSeen; }

  uint64_t observe(Core* core, uint32_t guestPc, uint32_t instruction) {
    if (!armed()) return 0;
    const uint64_t ordinal = ++mSeen;
    if (mPreFn) mPreFn(core, ordinal, guestPc, instruction, mUser);
    return ordinal;
  }

  void observePost(Core* core, uint64_t ordinal, uint32_t guestPc, uint32_t instruction) {
    if (ordinal != 0 && mPostFn) mPostFn(core, ordinal, guestPc, instruction, mUser);
  }

  template <typename Operation>
  void observeAround(Core* core, uint32_t guestPc, uint32_t instruction, Operation&& operation) {
    const uint64_t ordinal = observe(core, guestPc, instruction);
    operation();
    observePost(core, ordinal, guestPc, instruction);
  }

private:
  GtePreOpFn mPreFn = nullptr;
  GtePostOpFn mPostFn = nullptr;
  void* mUser = nullptr;
  uint64_t mSeen = 0;
};
