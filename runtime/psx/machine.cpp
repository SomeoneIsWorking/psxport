#include "machine.h"

#include "c_subsys.h" // mdec_init — the vendored MDEC device's own initializer
#include "core.h"
#include "dbg_server.h"
#include "execution_ledger.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call_census.h"
#include "host_input.h"
#include "hw_bind.h"
#include "render_mode.h"
#include "store_observe.h"

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

Core &Machine::core() const {
  return game_.core;
}

void Machine::bindDevices() {
  bindPerCoreDevices(game_);
}

void Machine::prepare() {
  if (!game_.runtime) {
    lucent::error("machine", "Game has no installed GameRuntime; refusing to compose a product boot");
    std::abort();
  }
  // The title's overrides first: the preflight below reports a product with no finite frame owner, and
  // a title's own registration is allowed to be part of answering that.
  game_.runtime->registerOverrides(game_);
  FrameLoopShell{}.prepareProduct(game_);
}

std::uint32_t Machine::attachControlChannel(std::uint32_t requestedFrameCap) {
  const int clientCap = game_.dbg_server.attach(&core(), static_cast<int>(requestedFrameCap));
  store_observe_attach(core());
  const std::uint32_t cap = clientCap > 0 ? static_cast<std::uint32_t>(clientCap) : 0u;
  lucent::info("machine",
               "live control channel {} (frame cap {}), store observer armed",
               debug_server_live() ? "attached" : "not requested",
               cap);
  return cap;
}

void Machine::stepFrame(std::uint32_t frame) {
  game_.dbg_server.honourPause(&core());
  FrameLoopShell{}.step(core(), frame);
  game_.dbg_server.service(&core());
}

void Machine::run(std::uint32_t frameCap) {
  // While this is alive a window close is a request this loop answers; without it the event drain
  // ends the process, which is what every product did before the request existed.
  const psx::input::QuitScope ownedQuit{game_.hostInput};
  lucent::info("machine", "entering the product frame loop ({})", frameCap ? "capped" : "interactive");
  for (std::uint32_t frame = 0; frameCap == 0u || frame < frameCap; ++frame) {
    stepFrame(frame);
    // TAKEN, not asked: this run ended because of that request, so leaving it set would end the next
    // run in this process on the first field.
    if (game_.hostInput.takeQuitRequest() || game_.dbg_server.takeQuitRequest()) {
      lucent::info("machine", "the run was asked to stop after {} field(s)", frame + 1u);
      break;
    }
  }
  // The whole-run guest ledger and the guest-call census, whatever ended the run: a product whose own
  // spine was abandoned, or whose loop returned, gets the same denominators as one that ended itself.
  psx::cpu::logRunEndLedger(core().lightrecExecutor().counters());
  core().guestCallCensus().log("product loop ended");
  lucent::info("machine", "product frame loop done");
}

} // namespace psx