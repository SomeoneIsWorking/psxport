#include "title_session.h"

#include "config.h"
#include "core.h"
#include "dbg_server.h"
#include "field_turn.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_device.h"
#include "host_input.h"
#include "host_turn.h"
#include "psx_exe_image.h"
#include "run_end_report.h"
#include "saved_disc.h"
#include "spu_beetle.h"
#include "state/state_command.h"
#include "store_observe.h"

#include <lucent/log.h>

#include <cstdlib>
#include <string>

namespace psx::host {
namespace {

// The process devices initialised, then bound to this Core through the shell, so its next step
// knows what is bound; then `a0`/`a1` as the BIOS leaves them.
void bindPerCoreDevices(Game &game, psx::frame::FrameLoopShell &shell) {
  Core &core = game.core;
  psx::audio::spu::init();
  shell.bindRunningCore(core);
  game.spu_audio.init();
  game.pad.init();
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
                           Role role,
                           bool selectorAvailable,
                           SessionServices &services)
    : catalog_(catalog), title_(title), role_(role), selectorAvailable_(selectorAvailable), services_(services),
      parts_(std::make_unique<Parts>()) {}

TitleSession::~TitleSession() {
  if (!parts_->booted) {
    return;
  }
  Game &game = *parts_->game;
  game.core.hostTurn.reset();
  if (role_ == Role::Product) {
    psx::RunEndReport{game}.emit(steps_);
  }
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
  // Panel defaults are applied only to a selector session. A product session must stay windowed
  // with the player's pad live and its audio on: a title whose boot waits on its own audio device
  // never gets there, and the run stalls in the first guest step with no frame ever presented.
  if (selectorAvailable_) {
    applyPresentRoute();
    // A pane host the picker attached while this panel was still cold.
    game.render().setPaneHost(std::move(pendingPaneHost_));
    pendingPaneHost_ = {};
    // The picker owns the player's input; a panel's guest must not see it (see setPlayerInput).
    game.pad.setPlayerInputSuppressed(!playerInput_);
    // Only the session the player is looking at is heard (see setAudible).
    game.spu_audio.output().setClaimed(audible_);
    game.session.setPreRolling(preRolling_);
  }

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
  services_.frameLoop.prepareProduct(game);
  // Several sessions each start the channel; only the first owns the host port, which is how the
  // picker and a running title share one.
  game.dbg_server.start();
  psx::cpu::configureStoreObserver(core);
  lucent::info("boot",
               "live control channel {}, store observer armed",
               psx::debug::debugServerLive() ? "attached" : "not requested");
  if (role_ == Role::Product) {
    // Black before the title builds, so its first frames never composite over a stale splash.
    game.display.build(psx::present::PresentCompletion::Transition);
    game.display.show();
  }
  runtime.startProgram(core);
  if (role_ == Role::Product && services_.configuredStatePending) {
    services_.configuredStatePending = false;
    if (std::string error; !psx::state::applyConfiguredState(core, error)) {
      lucent::error("boot", "{}", error);
      std::exit(1);
    }
  }
  // The play check measures the run, not the gap between the boot's clear and the first step.
  game.perf.restartPresentSeries();
  parts_->booted = true;
  lucent::info("session",
               "{} ({}) booted{}",
               title_.identity->displayName,
               title_.identity->serial,
               paused_ ? " — paused, waiting in its picker panel" : "");
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
  if (role_ != Role::Product) {
    return false;
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
  if (!parts_->booted || paused_ || end_ != End::Running) {
    return;
  }
  Game &game = *parts_->game;
  if (endRequested(game)) {
    return;
  }
  const bool product = role_ == Role::Product;
  if (product && game.config.repl && !game.repl.beforeStep(&game.core, static_cast<std::uint32_t>(steps_))) {
    end_ = End::Quit;
    return;
  }
  // `psx::frame::FieldTurn` is the framework's owner, so this loop cannot silently omit one of
  // the four services. What stays here is the body: this title's own finite frame step.
  const psx::frame::FieldTurn fieldTurn;
  fieldTurn.beginField(game.core);
  ++steps_;
  const unsigned long long pacedBefore = game.framePacer.pacedFields();
  const bool pastPrefix = game.requireFrameDriver().pastBootPrefix();
  const std::uint64_t presentsBefore = game.render().presentSerial();
  services_.frameLoop.step(game.core, static_cast<std::uint32_t>(steps_));
  showsTitlePicture_ = showsTitlePicture_ || (pastPrefix && game.core.gpuDevice().displayArea().enabled &&
                                              game.render().presentSerial() != presentsBefore);
  if (product) {
    services_.budget.step();
    game.repl.afterStep();
  }
  fieldTurn.endField(game.core, static_cast<std::uint32_t>(steps_));
  lastStepPacedFields_ = game.framePacer.pacedFields() - pacedBefore;
  endRequested(game);
}

bool TitleSession::returnRequested() const {
  return parts_->booted && parts_->game->session.returnRequested();
}

Core &TitleSession::core() {
  if (!parts_->game) {
    lucent::error("session",
                  "{} asked for its Core before boot() — a session has no machine until it has booted",
                  title_.identity->slug);
    std::abort();
  }
  return parts_->game->core;
}

psx::render::PresentedImage TitleSession::presentedPicture() const {
  if (!parts_->booted) {
    return {};
  }
  return parts_->game->render().lastPresented();
}

void TitleSession::applyPresentRoute() {
  if (!parts_->game) {
    return; // no machine yet; boot() applies it
  }
  if (paneDestination_) {
    parts_->game->render().presentToPane(paneImage_);
  } else {
    parts_->game->render().presentToWindow();
  }
}

void TitleSession::presentToPane(psx::present::PixelSize image) {
  paneDestination_ = true;
  paneImage_ = image;
  applyPresentRoute();
}

void TitleSession::setPaneHost(psx::present::PaneHost host) {
  if (parts_->game == nullptr) {
    // Kept until boot(): a panel is created before it boots, and the picker attaches its host then.
    pendingPaneHost_ = std::move(host);
    return;
  }
  parts_->game->render().setPaneHost(std::move(host));
}

void TitleSession::presentToWindow() {
  paneDestination_ = false;
  paneImage_ = {};
  applyPresentRoute();
}

void TitleSession::setPlayerInput(bool live) {
  playerInput_ = live;
  if (parts_->booted) {
    parts_->game->pad.setPlayerInputSuppressed(!live);
  }
}

void TitleSession::pause() {
  paused_ = true;
}

void TitleSession::resume() {
  paused_ = false;
}

void TitleSession::setAudible(bool audible) {
  audible_ = audible;
  if (parts_->booted) {
    parts_->game->spu_audio.output().setClaimed(audible);
  }
}

void TitleSession::setPreRolling(bool preRolling) {
  preRolling_ = preRolling;
  if (parts_->booted) {
    parts_->game->session.setPreRolling(preRolling);
  }
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
