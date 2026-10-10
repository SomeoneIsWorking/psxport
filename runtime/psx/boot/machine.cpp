#include "machine.h"

#include "c_subsys.h" // mdec_init — the vendored MDEC device's own initializer
#include "cfg.h"      // cfg_dump / cfg_on / cfg_str
#include "config.h"   // psx::config::report_once — arms the exit audit at BOOT, for every port
#include "core.h"
#include "crt0_boot.h" // crt0_setup — the derived guest crt0 group's applier
#include "dbg_server.h"
#include "execution_ledger.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_native_internal.h" // gpu_clear_display
#include "guest_call_census.h"
#include "host_input.h"
#include "hw_bind.h"
#include "memcensus.h"
#include "state/state_command.h"
#include "store_observe.h"

#include <cstdio>
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

Core &Machine::core() const {
  return game_.core;
}

void Machine::bindDevices() {
  bindPerCoreDevices(game_);
}

void Machine::bindSession() {
  Core &core = this->core();
  gte_bind(&core);
  core.rsub.projprim.bind(&core);
  spu_bind(&core);
  mdec_bind(&core);
  xa_bind(&core);
}

void Machine::reportConfigurationOnce() {
  psx::config::report_once();
}

void Machine::armHostDiagnostics() {
  memcensus_init();
  cfg_dump();
}

void Machine::playBootMovies() {
  // Intro FMVs: the real boot is SCEA (stub) -> Whoopee logo (LOGO.STR) -> opening movie (OP.STR) ->
  // title/menu. The game's own STR streaming times out under this runtime (its CD-streamed FMV sectors
  // never reach its StrPlayer), so the movies are played with the self-contained native FMV player.
  //
  // SPLIT OF OWNERSHIP: only LOGO.STR (which plays BEFORE the front-end overlay is even loaded) is
  // played at boot. OP.STR is owned by the front-end, whose DEMO menu machine states 4..7 ARE the OP.STR
  // sequence; playing OP here too made it play TWICE (boot + front-end) — the "FMV repeats" bug.
  //
  // PSXPORT_NO_FMV is the explicit diagnostic control, and `PSXPORT_NO_FMV=0` forces the movies back
  // on. What is not acceptable is inferring the intent from the render sink: headless is the same
  // pipeline with a different final sink, so a headless probe that silently skipped the movies could
  // not reproduce the failing behaviour it was sent to investigate.
  const bool skip = cfg_on("PSXPORT_NO_FMV");
  const char *override = cfg_str("PSXPORT_NO_FMV");
  const bool moviesPlay = (override != nullptr && *override != '\0' && atoi(override) == 0) ? true : !skip;
  const char *const *bootFmv = core().cfg ? core().cfg->bootFmv : nullptr;
  const int count = bootFmv ? static_cast<int>(sizeof core().cfg->bootFmv / sizeof core().cfg->bootFmv[0]) : 0;
  if (!moviesPlay) {
    lucent::warn("machine", "skipping intro FMVs (headless/NO_FMV)");
    return;
  }
  if (bootFmv == nullptr || bootFmv[0] == nullptr) {
    lucent::info("machine", "no boot FMV configured (GameConfig::bootFmv is empty) — nothing to play");
    return;
  }
  for (int i = 0; i < count && bootFmv[i] != nullptr; ++i) {
    lucent::info("machine", "playing boot FMV {}/{}: {}", i + 1, count, bootFmv[i]);
    game_.fmv.play(bootFmv[i]);
  }
}

void Machine::clearDisplayForFrontEnd() {
  gpu_clear_display(&core());
}

void Machine::setupGuestBoot() {
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
  fieldTurn_.beginField(core());
  psx::frame::FrameLoopShell{}.step(core(), frame);
  fieldTurn_.endField(core(), frame);
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
  reportRunEnd();
}

void Machine::reportRunEnd() {
  // The whole-run guest ledger and the guest-call census, whatever ended the run: a product whose own
  // spine was abandoned, or whose loop returned, gets the same denominators as one that ended itself.
  psx::cpu::logRunEndLedger(core().lightrecExecutor().counters());
  core().guestCallCensus().log("product loop ended");
  // PSXPORT_RAMDUMP: the AFTER-loop dump. This is not the diagnostic to reach for — a run that ends by
  // exiting its process loop never returns here, and a missing file from this knob is therefore not
  // evidence that RAM dumping is broken. PSXPORT_RAMDUMP_FRAME (psx::frame::FieldTurn, mid-run) is.
  const std::string &path = psx::config::cv_ramdump.get();
  if (path.empty()) {
    return;
  }
  FILE *dump = fopen(path.c_str(), "wb");
  if (dump == nullptr) {
    lucent::error("machine", "end-of-run RAM dump could not open {}", path);
    return;
  }
  fwrite(core().ram, 1, 0x200000, dump);
  fclose(dump);
  lucent::info("machine", "dumped 2MB RAM -> {}", path);
  lucent::info("machine", "product frame loop done");
}

} // namespace psx