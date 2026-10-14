#include "machine.h"

#include "c_subsys.h" // mdec_init — the vendored MDEC device's own initializer
#include "cfg.h"      // cfg_dump
#include "config.h"   // psx::config::report_once — arms the exit audit at BOOT, for every port
#include "core.h"
#include "crt0_boot.h" // crt0_setup — the derived guest crt0 group's applier
#include "dbg_server.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_native_internal.h" // gpu_clear_display
#include "host_input.h"
#include "hw_bind.h"
#include "memcensus.h"
#include "run_end_report.h"
#include "state/state_command.h"
#include "store_observe.h"
#include "title_catalog.h"

#include <cstdlib>

#include <lucent/log.h>

namespace psx {
namespace {

// The per-Core device binds, in the framework's measured order, and `a0`/`a1` as the BIOS leaves them.
//
// The order is load-bearing: `init` before `bind` for each device, and the GTE first because the
// native projection reads it. Every product that skipped one got a device whose state was never
// published to its own instance rather than an error.
void bindPerCoreDevices(Game &game) {
  Core &core = game.core;
  gte_init();
  gte_bind(&core);
  core.rsub.projprim.bind(&core);
  mdec_init();
  mdec_bind(&core);
  spu_init();
  spu_bind(&core);
  xa_bind(&core);
  game.spu_audio.init();
  game.gpu.gpu_native_init();
  game.cd.overridesInit();
  game.platform_hle.initBuiltins();
  game.pad.overridesInit();
  core.r[4] = 1;
  core.r[5] = 0;
}

} // namespace

Machine::Machine(Game &game) : game_(game) {}

void Machine::insertDisc(const psx::host::TitleIdentity &title) {
  game_.disc.env_key = title.discEnvVar;
}

Core &Machine::core() const {
  return game_.core;
}

void Machine::bindDevices() {
  bindPerCoreDevices(game_);
}

void Machine::reportConfigurationOnce() {
  psx::config::report_once();
}

void Machine::armHostDiagnostics() {
  memcensus_init();
  cfg_dump();
}

void Machine::clearDisplayForFrontEnd() {
  gpu_clear_display(&core());
}

std::string Machine::bootEntryRefusal(const GuestProgramImage *image) {
  if (image == nullptr) {
    return "the runtime declares no GuestProgramImage, so it has no bootEntry";
  }
  if (image->bootEntry == BootEntry::DriverOwned) {
    return {};
  }
  const Crt0Completeness crt0 = crt0Completeness(*image);
  if (crt0.unsetCount == 0) {
    return {};
  }
  return "bootEntry=Crt0 but " + std::to_string(crt0.unsetCount) + " of " + std::to_string(crt0.required) +
         " crt0 field(s) are unset (" + crt0.unset +
         "); a title whose frame driver enters the guest declares bootEntry=DriverOwned";
}

void Machine::setupGuestBoot() {
  const GuestProgramImage *image = core().guestProgramImage;
  if (image != nullptr && image->bootEntry == BootEntry::DriverOwned) {
    return;
  }
  crt0_setup(core());
}

bool Machine::tryApplyConfiguredState(std::string &error) const {
  return psx::state::applyConfiguredState(core(), error);
}

void Machine::applyConfiguredState() {
  std::string error;
  if (!tryApplyConfiguredState(error)) {
    lucent::error("machine", "{}", error);
    std::exit(1);
  }
}

void Machine::registerTitleOverrides() {
  if (!game_.runtime) {
    lucent::error("machine", "Game has no installed GameRuntime; refusing to compose a product boot");
    std::abort();
  }
  game_.runtime->registerOverrides(game_);
}

void Machine::prepareProduct() {
  psx::frame::FrameLoopShell{}.prepareProduct(game_);
}

void Machine::prepare() {
  // The title's overrides first: the preflight below reports a product with no finite frame owner, and
  // a title's own registration is allowed to be part of answering that.
  registerTitleOverrides();
  prepareProduct();
}

void Machine::attachControlChannel() {
  game_.dbg_server.start(&core());
  store_observe_attach(core());
  lucent::info(
      "machine", "live control channel {}, store observer armed", debug_server_live() ? "attached" : "not requested");
}

void Machine::stepFrame(std::uint32_t frame) {
  fieldTurn_.beginField(core());
  psx::frame::FrameLoopShell{}.step(core(), frame);
  fieldTurn_.endField(core(), frame);
}

void Machine::run(std::uint32_t frameCap) {
  // While attached a window close is a request this loop answers; without it the drain ends the
  // process.
  psx::input::QuitScope quit;
  game_.hostInput.attachQuitScope(&quit);
  lucent::info("machine", "entering the product frame loop ({})", frameCap ? "capped" : "interactive");
  std::uint32_t frame = 0;
  for (; frameCap == 0u || frame < frameCap; ++frame) {
    stepFrame(frame);
    if (quit.take() || game_.dbg_server.takeQuitRequest()) {
      lucent::info("machine", "the run was asked to stop after {} field(s)", frame + 1u);
      ++frame;
      break;
    }
  }
  game_.hostInput.attachQuitScope(nullptr);
  RunEndReport{game_}.emit(frame);
}

} // namespace psx
