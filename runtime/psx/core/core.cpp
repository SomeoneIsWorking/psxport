// Core::Core / Core::~Core — the per-instance R3000 machine's constructor and destructor.
//
// Zero the R3000 register bank + main-RAM + scratchpad, allocate the render subsystem umbrella
// (`class Render` — game/render/render.h), and wire every owned subsystem's back-pointer to `this`
// so its methods can reach this Core's guest memory. Callers access subsystems as:
//     c->screenFade.method(args)   // embedded-value subsystems
//     c->mRender->mNodeXform.method(args)   // pointer-to-umbrella subsystem
//
// Lived in mem.cpp historically (right next to the memory-access primitives) — moved out into its
// own file so Core lifetime concerns aren't tangled with the memory-window helpers.
#include "core.h"

#include "config_vars.h"
#include "execution_control.h"
#include "function_reach.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include "override_differential.h"
#include "render_capabilities.h"
#include <cstring>
#include <lucent/log.h>
#include <utility>

Core::Core() {
  memset((R3000 *)this, 0, sizeof(R3000));
  memset(ram, 0, sizeof(ram));
  memset(scratch, 0, sizeof(scratch));
  executionControl_ = std::make_unique<psx::cpu::ExecutionControl>();
  imageCatalog_ = std::make_unique<psx::cpu::ImageCatalog>();
  lightrecExecutor_ = std::make_unique<psx::cpu::LightrecExecutor>(*this, psx::config::lightrec_fallback_policy);
  nativeDispatcher_ = std::make_unique<psx::cpu::NativeDispatcher>(*this);
  // The override differential is armed here, on EVERY runtime's route, because a gate armed only on
  // one spine is a gate some products silently never reach (store_observe.h records that incident).
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
  // Snapshot the game-owned polymorphic runtime. The two legacy views are non-null only when the
  // bounded adapter was installed by a consumer that has not migrated this seam yet.
  runtime = psxport_game_runtime();
  guestProgramImage = runtime ? runtime->guestProgramImage() : nullptr;
  cfg = psxport_game_config();
  hooks = psxport_game_hooks();
  if (runtime) {
    gameCtx = runtime->createContext(*this);
  }
}

Core::~Core() {
  // Any live `render path` switch was addressed to THIS Core; let go of it before the machine goes,
  // so the process-global that remembers it cannot outlive the pointer it holds.
  render_path_forget(this);
  if (runtime) {
    runtime->destroyContext(gameCtx);
  }
  gameCtx = nullptr;
}

psx::cpu::ExecutionControl &Core::executionControl() {
  return *executionControl_;
}

psx::cpu::ImageCatalog &Core::imageCatalog() {
  return *imageCatalog_;
}

psx::cpu::LightrecExecutor &Core::lightrecExecutor() {
  return *lightrecExecutor_;
}

psx::cpu::NativeDispatcher &Core::nativeDispatcher() {
  return *nativeDispatcher_;
}

std::optional<psx::cpu::ImageIdentity> Core::currentImageIdentity(uint32_t guestAddress) const {
  return imageCatalog_->resolve(guestAddress);
}

std::optional<psx::cpu::ImageIdentity> Core::currentImageIdentity(GuestAddressRange physicalRange) const {
  return imageCatalog_->resolve(physicalRange);
}
