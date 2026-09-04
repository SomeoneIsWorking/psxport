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
#include "game_runtime.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include <cstring>

Core::Core() {
  memset((R3000 *)this, 0, sizeof(R3000));
  memset(ram, 0, sizeof(ram));
  memset(scratch, 0, sizeof(scratch));
  executionControl_ = std::make_unique<psx::cpu::ExecutionControl>();
  imageCatalog_ = std::make_unique<psx::cpu::ImageCatalog>();
  lightrecExecutor_ = std::make_unique<psx::cpu::LightrecExecutor>(*this, psx::config::lightrec_fallback_policy);
  nativeDispatcher_ = std::make_unique<psx::cpu::NativeDispatcher>(*this);
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
