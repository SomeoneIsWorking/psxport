#include "platform_hle.h"

#include "core.h"
#include "game.h"
#include "stock_vsync.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>

void PlatformHle::completeImmediately(Core *core) {
  core->r[psx::cpu::kV0] = 0;
}

void PlatformHle::bindStock(std::uint32_t address, psx::cpu::NativeFunction function) {
  if (address && !insert(address, function)) {
    std::abort();
  }
}

bool PlatformHle::insert(std::uint32_t address, psx::cpu::NativeFunction function) {
  for (int index = 0; index < mN; ++index) {
    if (mAddr[index] == address) {
      if (address == vsyncAddress() && mFn[index] != function) {
        lucent::error("plat-hle", "refused replacement of the VSync frame boundary at 0x{:08X}", address);
        return false;
      }
      mFn[index] = function;
      game->core.dispatchEpoch.advance();
      return true;
    }
  }
  if (mN == kMax) {
    lucent::error("plat-hle", "hardware-service table is full while registering 0x{:08X}", address);
    return false;
  }
  mAddr[mN] = address;
  mFn[mN] = function;
  ++mN;
  mLo = std::min(mLo, address);
  mHi = std::max(mHi, address);
  game->core.dispatchEpoch.advance();
  return true;
}

namespace {

void vsyncEntry(Core *core) {
  core->game->platform_hle.vsync()->serve(*core);
}

} // namespace

void PlatformHle::bindVSync(const psx::hle::StockVSync &vsync) {
  const std::uint32_t address = vsync.entry();
  if (!address) {
    return;
  }
  if (mVSync && (mVSync->entry() & 0x1fffffffu) != (address & 0x1fffffffu)) {
    lucent::error("plat-hle", "conflicting VSync addresses 0x{:08X} and 0x{:08X}", mVSync->entry(), address);
    std::abort();
  }
  if (mVSync && mVSync->queryCounter() != vsync.queryCounter()) {
    lucent::error("plat-hle",
                  "conflicting VSync query counters 0x{:08X} and 0x{:08X}",
                  mVSync->queryCounter(),
                  vsync.queryCounter());
    std::abort();
  }
  const psx::hle::StockVSync *previous = mVSync;
  mVSync = &vsync;
  if (!insert(address, vsyncEntry)) {
    mVSync = previous;
    std::abort();
  }
}

std::uint32_t PlatformHle::vsyncAddress() const {
  return mVSync ? mVSync->entry() : 0u;
}

void PlatformHle::requireNativeFrameLoopContract() const {
  if (!vsyncAddress()) {
    lucent::error("plat-hle", "no measured VSync address; refusing guest execution");
    std::abort();
  }
}

psx::cpu::NativeFunction PlatformHle::lookup(std::uint32_t address) const {
  if (address < mLo || address > mHi) {
    return nullptr;
  }
  for (int index = 0; index < mN; ++index) {
    if (mAddr[index] == address) {
      return mFn[index];
    }
  }
  return nullptr;
}
