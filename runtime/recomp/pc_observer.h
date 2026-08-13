#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

class Core;
using PcObserverFn = void (*)(Core*, uint64_t ordinal, uint32_t guestPc, void* user);

class PcObserver {
public:
  static constexpr size_t kMaxTargets = 8;

  bool arm(const uint32_t* targets, size_t count, PcObserverFn fn, void* user) {
    disarm();
    mSeen = mMatched = 0;
    if (!fn || !targets || count == 0 || count > kMaxTargets) return false;
    for (size_t i = 0; i < count; ++i) mTargets[i] = targets[i];
    mTargetCount = count; mFn = fn; mUser = user;
    return true;
  }
  bool matches(uint32_t pc) const {
    if (!mFn) return false;
    for (size_t i = 0; i < mTargetCount; ++i)
      if (mTargets[i] == pc) return true;
    return false;
  }
  uint64_t observe(Core* core, uint32_t pc) {
    if (!mFn) return 0;
    const uint64_t ordinal = ++mSeen;
    if (matches(pc)) { ++mMatched; mFn(core, ordinal, pc, mUser); }
    return ordinal;
  }
  void disarm() { mFn = nullptr; mUser = nullptr; mTargetCount = 0; }
  bool armed() const { return mFn != nullptr; }
  uint64_t seen() const { return mSeen; }
  uint64_t matched() const { return mMatched; }
private:
  std::array<uint32_t, kMaxTargets> mTargets{};
  size_t mTargetCount = 0;
  PcObserverFn mFn = nullptr;
  void* mUser = nullptr;
  uint64_t mSeen = 0, mMatched = 0;
};
