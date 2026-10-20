#include "core.h"

#include "config_vars.h"
#include "execution_control.h"
#include "function_reach.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_device.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include "override_differential.h"

#include <lucent/log.h>

#include <cstring>
#include <utility>

Core::Core(psx::game::GameRuntime &runtime) : runtime(runtime), gpuDevice_(std::make_unique<psx::gpu::GpuDevice>()) {
  std::memset(static_cast<R3000 *>(this), 0, sizeof(R3000));
  std::memset(ram, 0, sizeof(ram));
  std::memset(scratch, 0, sizeof(scratch));
  executionControl_ = std::make_unique<psx::cpu::ExecutionControl>();
  imageCatalog_ = std::make_unique<psx::cpu::ImageCatalog>();
  lightrecExecutor_ = std::make_unique<psx::cpu::LightrecExecutor>(*this, psx::config::lightrec_fallback_policy);
  nativeDispatcher_ = std::make_unique<psx::cpu::NativeDispatcher>(*this);
  // Armed for every runtime, so no product route can miss the gate.
  if (psx::cpu::OverrideDifferentialConfig differential = psx::config::override_differential_config();
      differential.error) {
    lucent::error("override-diff", "REFUSED, not armed: {}", *differential.error);
  } else if (differential.enabled()) {
    nativeDispatcher_->attachDifferential(
        std::make_unique<psx::cpu::OverrideDifferential>(*this, std::move(differential)));
  }
  if (std::string reachReport = psx::config::function_reach_report_path(); !reachReport.empty()) {
    lightrecExecutor_->attachFunctionReach(
        std::make_unique<psx::cpu::FunctionReach>(*imageCatalog_, std::move(reachReport)));
  }
  storeWatch.configure();
}

Core::~Core() = default;

psx::cpu::ExecutionControl &Core::executionControl() {
  return *executionControl_;
}

psx::cpu::ImageCatalog &Core::imageCatalog() {
  return *imageCatalog_;
}

const psx::cpu::ImageCatalog &Core::imageCatalog() const {
  return *imageCatalog_;
}

psx::cpu::LightrecExecutor &Core::lightrecExecutor() {
  return *lightrecExecutor_;
}

psx::cpu::NativeDispatcher &Core::nativeDispatcher() {
  return *nativeDispatcher_;
}

// The CD and SIO edges are folded in on every I_STAT access, so neither waits for a later look.
uint32_t Core::irqStatLatch() {
  game->sio.service();
  if (game->cdc.irq_edge) {
    game->cdc.irq_edge = 0;
    raiseInterrupt(IRQ_BIT_CD);
  }
  return game->hle.i_stat;
}

void Core::raiseInterrupt(IrqBit bit) {
  game->hle.i_stat |= 1u << bit;
  pending_work |= PW_IRQ;
  lucent::debug("irq",
                "IRQ{} raised -> I_STAT=0x{:03X} (mask=0x{:03X}, {})",
                static_cast<unsigned>(bit),
                game->hle.i_stat,
                game->hle.i_mask,
                (game->hle.i_mask & (1u << bit)) != 0u ? "enabled" : "masked by the guest");
}

uint32_t Core::irqPendingEnabled() {
  return irqStatLatch() & game->hle.i_mask;
}

bool Core::irqPendingEnabled(IrqBit bit) {
  return (irqPendingEnabled() & (1u << bit)) != 0u;
}
