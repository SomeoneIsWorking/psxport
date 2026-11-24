#include "title_session.h"

#include "config.h"
#include "core.h"
#include "dbg_server.h"
#include "executor_trace.h"
#include "field_turn.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "host_input.h"
#include "host_turn.h"
#include "psx_exe_image.h"
#include "run_end_report.h"
#include "saved_disc.h"
#include "state/state_command.h"
#include "store_observe.h"

#include <lucent/log.h>

#include <cstdlib>
#include <string>

namespace psx::host {
namespace {

// This Core's devices bound through the shell (each powers on at its first bind), so its next step
// knows what is bound; then `a0`/`a1` as the BIOS leaves them.
void bindPerCoreDevices(Game &game, psx::frame::FrameLoopShell &shell) {
  Core &core = game.core;
  shell.bindRunningCore(core);
  game.spu_audio.init();
  game.pad.init();
  // The retail kernel boots with "bu" in its device table; libmcrd's directory wait needs it.
  game.memcard.install();
  core.r[4] = 1;
  core.r[5] = 0;
}

} // namespace

// The session's state that needs the framework's headers, hidden so this header stays the
// session's contract.
struct TitleSession::Parts {
  // Built in boot(), after the boot entry and the executable are accepted.
  std::unique_ptr<Game> game;
  bool booted = false;
};

TitleSession::TitleSession(const TitleCatalog &catalog,
                           const TitleAvailability &title,
                           bool selectorAvailable,
                           SessionServices &services)
    : catalog_(catalog), title_(title), selectorAvailable_(selectorAvailable), services_(services),
      parts_(std::make_unique<Parts>()) {}

TitleSession::~TitleSession() {
  if (!parts_->booted) {
    return;
  }
  Game &game = *parts_->game;
  game.core.hostTurn.reset();
  psx::RunEndReport{game}.emit(steps_);
  services_.frameLoop.forgetCore(game.core);
}

bool TitleSession::boot() {
  if (parts_->booted) {
    return true;
  }
  psx::game::GameRuntime &runtime = catalog_.runtime(title_.index);
  if (!title_.available()) {
    lucent::error("boot", "{} has no authenticated executable", title_.identity->displayName);
    return false;
  }
  parts_->game = std::make_unique<Game>(runtime, services_.presentation, services_.input, services_.config);
  Game &game = *parts_->game;
  Core &core = game.core;
  game.session.setReturnAvailable(selectorAvailable_);
  game.productName = catalog_.productName();
  game.title = title_.identity;
  game.executable = title_.executable;
  SavedDisc(*title_.identity).bind(game.disc);
  // Open now, so the first guest read does not pay for the image's hunk map inside a field.
  game.disc.open();
  const psx::cpu::PsxExeLoadResult admitted = runtime.admitExecutable(core, title_.bytes);
  if (!admitted) {
    lucent::error("boot",
                  "{} refused its executable {}: {}",
                  title_.identity->displayName,
                  title_.executable.string(),
                  admitted.detail);
    return false;
  }
  lucent::info("boot",
               "admitted {}: entry 0x{:08X} load 0x{:08X} text 0x{:X} sp 0x{:08X}",
               title_.executable.string(),
               admitted.image.entry,
               admitted.image.textAddress,
               admitted.image.textBytes,
               core.r[29]);
  psx::config::report_once();
  bindPerCoreDevices(game, services_.frameLoop);
  // The overrides before the preflight: a title's own registration may be what gives it a finite
  // frame owner.
  runtime.registerOverrides(game);
  game.mods.offerNativeDepth(core.depthTest.marksAny());
  game.mods.offerCrop(runtime.cropUiLayers(core) != nullptr);
  services_.frameLoop.prepareProduct(game);
  // The selector's Game started the channel first; this session claims it after boot (claimDebugEndpoint).
  game.dbg_server.start();
  psx::cpu::configureStoreObserver(core);
  psx::cpu::configureExecutorDiagnostics(core);
  lucent::info("boot",
               "live control channel {}, store observer armed",
               psx::debug::debugServerLive() ? "attached" : "not requested");
  // Black before the title builds, so its first frames never composite over a stale splash.
  game.display.build(psx::present::PresentCompletion::Transition);
  game.display.show();
  runtime.startProgram(core);
  if (services_.configuredStatePending) {
    services_.configuredStatePending = false;
    if (std::string error; !psx::state::applyConfiguredState(core, error)) {
      lucent::error("boot", "{}", error);
      std::exit(1);
    }
  }
  // The play check measures the run, not the gap between the boot's clear and the first step.
  game.perf.restartPresentSeries();
  parts_->booted = true;
  lucent::info("session", "{} ({}) booted", title_.identity->displayName, title_.identity->serial);
  return true;
}

bool TitleSession::endRequested(Game &game) {
  if (services_.presentation.faulted()) {
    end_ = End::DrawFault;
    return true;
  }
  if (game.session.returnRequested()) {
    end_ = End::ReturnedToSelector;
    return true;
  }
  if (game.run.shouldEnd()) {
    lucent::info("session", "{} ended its run after {} step(s)", title_.identity->displayName, steps_);
    end_ = End::TitleEnded;
    return true;
  }
  if (services_.quit.take() || game.dbg_server.takeQuitRequest()) {
    lucent::info("session", "{} was asked to quit after {} step(s)", title_.identity->displayName, steps_);
    end_ = End::Quit;
    return true;
  }
  if (services_.budget.exhausted()) {
    lucent::info("session", "{} reached its run budget of {} step(s)", title_.identity->displayName, steps_);
    end_ = End::BudgetSpent;
    return true;
  }
  return false;
}

void TitleSession::step() {
  if (!parts_->booted || end_ != End::Running) {
    return;
  }
  Game &game = *parts_->game;
  if (endRequested(game)) {
    return;
  }
  if (game.config.repl && !game.repl.beforeStep(&game.core, static_cast<std::uint32_t>(steps_))) {
    end_ = End::Quit;
    return;
  }
  // `psx::frame::FieldTurn` is the framework's owner, so this loop cannot silently omit one of
  // the four services. What stays here is the body: this title's own finite frame step.
  const psx::frame::FieldTurn fieldTurn;
  fieldTurn.beginField(game.core);
  ++steps_;
  services_.frameLoop.step(game.core, static_cast<std::uint32_t>(steps_));
  services_.budget.step();
  game.repl.afterStep();
  fieldTurn.endField(game.core, static_cast<std::uint32_t>(steps_));
  endRequested(game);
}

void TitleSession::claimDebugEndpoint() {
  if (!parts_->booted) {
    return;
  }
  if (!parts_->game->dbg_server.claimEndpoint()) {
    lucent::warn(
        "host", "another session still holds the debug endpoint; {} cannot claim it", title_.identity->displayName);
  }
}

} // namespace psx::host
