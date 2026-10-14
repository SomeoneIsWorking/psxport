#include "platform_hle.h"

#include "core.h"
#include "game.h"
#include "scheduler.h"
#include "stock_vsync.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>

namespace {
enum Register : int { V0 = 2, A0 = 4, A1 = 5 };

// The legacy group's CdReadSync: zeroes the 8-byte result and reports completion.
void legacyCdReadSync(Core *core) {
  const std::uint32_t result = core->r[A1];
  for (std::uint32_t index = 0; result != 0 && index < 8; ++index) {
    core->mem_w8(result + index, 0);
  }
  core->r[V0] = 0;
}

} // namespace

void PlatformHle::completeImmediately(Core *core) {
  core->r[V0] = 0;
}

bool PlatformHle::inBiosWindow(const GameConfig &config, std::uint32_t address) {
  for (int index = 0; index < kPlatformHleWindowCapacity; ++index) {
    if (config.hle.windowHi[index] != 0 && address >= config.hle.windowLo[index] &&
        address < config.hle.windowHi[index]) {
      return true;
    }
  }
  lucent::error("plat-hle", "no configured hardware-service window admits 0x{:08X}", address);
  return false;
}

bool PlatformHle::register_(std::uint32_t address, OverrideFn function) {
  if (!function || !game || !game->core.cfg || !inBiosWindow(*game->core.cfg, address)) {
    return false;
  }
  return insert(address, function);
}

void PlatformHle::bindStock(std::uint32_t address, OverrideFn function) {
  if (address && !insert(address, function)) {
    std::abort();
  }
}

bool PlatformHle::insert(std::uint32_t address, OverrideFn function) {
  for (int index = 0; index < mN; ++index) {
    if (mAddr[index] == address) {
      if (address == vsyncAddress() && mFn[index] != function) {
        lucent::error("plat-hle", "refused replacement of the VSync frame boundary at 0x{:08X}", address);
        return false;
      }
      mFn[index] = function;
      ++mRevision;
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
  ++mRevision;
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

OverrideFn PlatformHle::lookup(std::uint32_t address) const {
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

void PlatformHle::initBuiltins() {
  const GameConfig *legacy = game->core.cfg;
  if (!legacy) {
    return;
  }
  const GameConfig::PlatformHleCfg &config = legacy->hle;
  auto install = [this](std::uint32_t address, OverrideFn function) {
    if (address && !register_(address, function)) {
      std::abort();
    }
  };
  install(config.decDctInSync, completeImmediately);
  install(config.decDctOutSync, completeImmediately);
  install(config.cdReadSync, legacyCdReadSync);
  install(config.cdDataSync, completeImmediately);
  install(config.cdInitHandshake, completeImmediately);
  install(config.changeThread, scheduler_yield);
  lucent::info("plat-hle", "{} legacy-adapter hardware services installed", mN);
}
