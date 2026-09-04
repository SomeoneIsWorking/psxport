#include "platform_hle.h"

#include "cd_control.h"
#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "proj_params.h"
#include "scheduler.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>

namespace {
enum Register : int { V0 = 2, A0 = 4, A1 = 5 };

void setGeomOffset(Core *core) {
  const auto x = static_cast<std::int32_t>(core->r[A0]);
  const auto y = static_cast<std::int32_t>(core->r[A1]);
  core->r[A0] = static_cast<std::uint32_t>(x) << 16;
  core->r[A1] = static_cast<std::uint32_t>(y) << 16;
  libgte_set_geom_offset(core, x, y);
}

void setGeomScreen(Core *core) {
  libgte_set_geom_screen(core, static_cast<std::int32_t>(core->r[A0]));
}

void syncComplete(Core *core) {
  core->r[V0] = 0;
}

void zeroResult(Core *core, std::uint32_t address) {
  for (int index = 0; address != 0 && index < 8; ++index) {
    core->mem_w8(address + static_cast<std::uint32_t>(index), 0);
  }
}

void cdReadSync(Core *core) {
  zeroResult(core, core->r[A1]);
  core->r[V0] = 0;
}

void gpuTimeoutArm(Core *core) {
  if (!core->cfg) {
    return;
  }
  if (core->cfg->hle.gpuTimeoutDeadlineVar) {
    core->mem_w32(core->cfg->hle.gpuTimeoutDeadlineVar, 0x7fffffffu);
  }
  if (core->cfg->hle.gpuTimeoutFlagVar) {
    core->mem_w32(core->cfg->hle.gpuTimeoutFlagVar, 0);
  }
}

void frameBoundary(Core *core) {
  psx::cpu::requestExecutionExit(*core, psx::cpu::ExecutionExitReason::FrameBoundary);
}
} // namespace

bool PlatformHle::inBiosWindow(const GameConfig *config, std::uint32_t address) {
  if (!config) {
    const GameRuntime *runtime = psxport_game_runtime();
    const PlatformHlePlan *plan = runtime ? runtime->platformHlePlan() : nullptr;
    if (plan) {
      for (int index = 0; index < kPlatformHleWindowCapacity; ++index) {
        if (plan->windowHi[index] != 0 && address >= plan->windowLo[index] && address < plan->windowHi[index]) {
          return true;
        }
      }
    }
    lucent::error("plat-hle", "no direct-runtime hardware-service window admits 0x{:08X}", address);
    return false;
  }

  for (int index = 0; index < kPlatformHleWindowCapacity; ++index) {
    if (config->hle.windowHi[index] != 0 && address >= config->hle.windowLo[index] &&
        address < config->hle.windowHi[index]) {
      return true;
    }
  }
  lucent::error("plat-hle", "no configured hardware-service window admits 0x{:08X}", address);
  return false;
}

bool PlatformHle::register_(std::uint32_t address, OverrideFn function) {
  if (!function || !game || !inBiosWindow(game->core.cfg, address)) {
    return false;
  }
  for (int index = 0; index < mN; ++index) {
    if (mAddr[index] == address) {
      if (address == mVSyncAddress && mFn[index] != function) {
        lucent::error("plat-hle", "refused replacement of the VSync frame boundary at 0x{:08X}", address);
        return false;
      }
      mFn[index] = function;
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
  return true;
}

void PlatformHle::bindVSyncBoundary(std::uint32_t address) {
  if (!address) {
    return;
  }
  if (mVSyncAddress && (mVSyncAddress & 0x1fffffffu) != (address & 0x1fffffffu)) {
    lucent::error("plat-hle", "conflicting VSync addresses 0x{:08X} and 0x{:08X}", mVSyncAddress, address);
    std::abort();
  }
  mVSyncAddress = address;
  if (!register_(address, frameBoundary)) {
    mVSyncAddress = 0;
    std::abort();
  }
}

void PlatformHle::requireNativeFrameLoopContract() const {
  if (!mVSyncAddress) {
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
  auto install = [this](std::uint32_t address, OverrideFn function) {
    if (address && !register_(address, function)) {
      std::abort();
    }
  };
  auto installProjection = [&](std::uint32_t offset, std::uint32_t screen) {
    install(offset, setGeomOffset);
    install(screen, setGeomScreen);
  };

  if (!game->core.cfg) {
    const GameRuntime *runtime = psxport_game_runtime();
    const PlatformHlePlan *plan = runtime ? runtime->platformHlePlan() : nullptr;
    if (!plan) {
      lucent::error("plat-hle", "direct runtime declares no PlatformHlePlan");
      return;
    }
    installProjection(plan->setGeomOffset, plan->setGeomScreen);
    install(plan->cdReadAddress, cd_read_stock_sync);
    install(plan->cdReadSyncAddress, cd_readsync_stock_sync);
    install(plan->drawSyncAddress, syncComplete);
    bindVSyncBoundary(plan->vsyncAddress);
    for (int index = 0; index < plan->bindingCount && index < PlatformHlePlan::kMaxBindings; ++index) {
      install(plan->bindings[index].addr, plan->bindings[index].fn);
    }
    lucent::info("plat-hle", "{} direct-runtime hardware services installed", mN);
    return;
  }

  const GameConfig::PlatformHleCfg &config = game->core.cfg->hle;
  install(config.decDctInSync, syncComplete);
  install(config.decDctOutSync, syncComplete);
  install(config.cdReadSync, cdReadSync);
  install(config.cdDataSync, syncComplete);
  install(config.cdInitHandshake, syncComplete);
  install(config.gpuTimeoutArm, gpuTimeoutArm);
  install(config.gpuTimeoutCheck, syncComplete);
  install(config.drawSync, syncComplete);
  install(config.changeThread, scheduler_yield);
  bindVSyncBoundary(config.vsyncTrap);
  installProjection(config.setGeomOffset, config.setGeomScreen);
  lucent::info("plat-hle", "{} legacy-adapter hardware services installed", mN);
}
