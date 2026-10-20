// Per-Game table of the stock hardware-sync leaves (VSync, CdReadSync, MDEC and GPU waits) the Stock* classes bind.
#pragma once
#include "native_function.h"

#include <cstdint>
class Core;
class Game;
namespace psx::hle {
class StockLibgpu;
class StockVSync;
} // namespace psx::hle

class PlatformHle {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  // A stock library entry; zero `addr` is an entry the title has not located. Aborts on a full table.
  void bindStock(uint32_t addr, psx::cpu::NativeFunction fn);
  // The libetc VSync entry as the protected native frame boundary.
  void bindVSync(const psx::hle::StockVSync &vsync);
  const psx::hle::StockVSync *vsync() const {
    return mVSync;
  }
  // The title's libgpu, whose timeout arm this table dispatches.
  void bindLibgpu(const psx::hle::StockLibgpu &libgpu) {
    mLibgpu = &libgpu;
  }
  const psx::hle::StockLibgpu *libgpu() const {
    return mLibgpu;
  }
  // Completes a hardware wait the host already finished: v0 = 0.
  static void completeImmediately(Core *core);

  // Product preflight. A missing measured VSync address would let a retail busy-wait run and report
  // a misleading timeout, so boot refuses before title initialization can enter guest main.
  void requireNativeFrameLoopContract() const;

  [[nodiscard]] uint32_t vsyncAddress() const;

  // Fast lookup — called on every guest call target. Uses a [min,max] gate for the common case.
  // Returns nullptr for a miss.
  psx::cpu::NativeFunction lookup(uint32_t addr) const;

  // Advances whenever a registration is added or replaced, so a cache of lookup answers can tell that
  // it is stale without repeating the lookup.
  [[nodiscard]] uint64_t revision() const {
    return mRevision;
  }

private:
  bool insert(uint32_t addr, psx::cpu::NativeFunction fn);

  static constexpr int kMax = 32;

  uint32_t mAddr[kMax] = {0};
  psx::cpu::NativeFunction mFn[kMax] = {nullptr};
  int mN = 0;
  uint32_t mLo = 0xFFFFFFFFu;
  uint32_t mHi = 0;
  const psx::hle::StockVSync *mVSync = nullptr;
  const psx::hle::StockLibgpu *mLibgpu = nullptr;
  uint64_t mRevision = 0;
};
