#include "title_session.h"

#include "cfg.h"
#include "core.h"
#include "dbg_server.h"
#include "field_turn.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_vk.h"
#include "gpu_vk_internal.h"
#include "host_turn.h"
#include "hw_bind.h"
#include "lightrec_executor.h"
#include "machine.h"
#include "psx_exe_image.h"

#include <lucent/log.h>

#include <cstdlib>

extern "C" {
void watchdog_init(void);
void watchdog_disable(void);
void mdec_init(void);
void spu_init(void);
}

void dc_boot_init(Core *c);
void dc_step_frame(Core *c, uint32_t frame);

namespace psx::host {

// The session's state that needs the framework's headers, hidden so this header stays the
// session's contract.
struct TitleSession::Parts {
  // The Game is built in boot(), not here: a Core snapshots the process's installed GameRuntime
  // when it is constructed, so the title's runtime is installed immediately before its own Game
  // exists.
  std::unique_ptr<Game> game;
  bool booted = false;
};

TitleSession::TitleSession(const TitleCatalog &catalog,
                           const TitleAvailability &title,
                           bool selectorAvailable,
                           GpuDevice &presentation)
    : catalog_(catalog), title_(title), selectorAvailable_(selectorAvailable), presentation_(presentation),
      parts_(std::make_unique<Parts>()) {}

TitleSession::~TitleSession() {
  if (!parts_->booted) {
    return;
  }
  // The frame alarm must not outlive its title into the next session or the selector.
  watchdog_disable();
  psx::cpu::shutdownHostTurn(parts_->game->core);
  reportRun(parts_->game->core);
}

void TitleSession::reportRun(Core &core) const {
  const auto &counts = core.lightrecExecutor().counters();
  lucent::info("runtime",
               "run complete: fields={} product_steps={} presentation_fences={} "
               "translated_blocks={} executed_blocks={} executed_instructions={} "
               "cache_hits={} cache_misses={} host_dispatches={} invalidations={} faults={}",
               parts_->game->run.fields(),
               steps_,
               core.game->presentation.fence(),
               counts.translatedBlocks,
               counts.executedBlocks,
               counts.executedInstructions,
               counts.cacheHits,
               counts.cacheMisses,
               counts.hostDispatches,
               counts.invalidations,
               counts.faults);
  core.lightrecExecutor().reportFallbackTelemetry("run-complete");
  core.runtime->reportRun(core);
}

bool TitleSession::boot() {
  if (parts_->booted) {
    return true;
  }
  psxport_install_game(catalog_.runtime(title_.index));
  parts_->game = std::make_unique<Game>(presentation_);
  Game &game = *parts_->game;
  Core &core = game.core;
  game.session.setReturnAvailable(selectorAvailable_);
  // Panel defaults are applied only to a selector session. A product session must stay windowed
  // with the player's pad live and its audio on: a title whose boot waits on its own audio device
  // never gets there, and the run stalls in the first guest step with no frame ever presented.
  if (selectorAvailable_) {
    applyPresentRoute();
    // The picker owns the player's input; a panel's guest must not see it (see setPlayerInput).
    game.pad.setPlayerInputSuppressed(!playerInput_);
    // Only the session the player is looking at is heard (see setAudible).
    game.spu_audio.setOutputEnabled(audible_);
  }

  watchdog_init();
  load_exe(title_.executable.c_str(), &core);
  // Power, done once. The per-frame-step rebind that names the session actually running is
  // FrameLoopShell::step's own job.
  gte_bind(&core);
  spu_bind(&core);
  mdec_bind(&core);
  xa_bind(&core);
  gte_init();
  mdec_init();
  spu_init();
  game.spu_audio.init();
  game.gpu.gpu_native_init();

  // Several sessions each attach; only the first owns the host port, which is how the
  // picker and a running title share one channel.
  const std::uint32_t frameCap = psx::Machine{game}.attachControlChannel(cfg_int("PSXPORT_NATIVE_FRAMES", 0));
  game.run = SessionRun(frameCap);

  dc_boot_init(&core);
  parts_->booted = true;
  lucent::info("session",
               "{} ({}) booted{}",
               title_.identity->displayName,
               title_.identity->serial,
               paused_ ? " — paused, waiting in its picker panel" : "");
  return true;
}

void TitleSession::step() {
  if (!parts_->booted || paused_) {
    return;
  }
  Game &game = *parts_->game;
  if (game.session.returnRequested()) {
    end_ = End::ReturnedToSelector;
    return;
  }
  // `psx::frame::FieldTurn` is the framework's owner, so this loop cannot silently omit one of
  // the four services. What stays here is the body: this title's own finite frame step.
  const psx::frame::FieldTurn fieldTurn;
  fieldTurn.beginField(game.core);
  if (game.run.shouldEnd()) {
    fieldTurn.endField(game.core, static_cast<std::uint32_t>(steps_));
    end_ = End::Finished;
    return;
  }
  dc_step_frame(&game.core, static_cast<uint32_t>(++steps_));
  fieldTurn.endField(game.core, static_cast<std::uint32_t>(steps_));
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

bool TitleSession::showsTitlePicture() const {
  if (!parts_->booted || parts_->game == nullptr) {
    return false;
  }
  // The driver knows when the retail boot prefix has returned; a pixel test cannot tell an
  // animated publisher logo from an attract demo.
  const FrameDriver &driver = psx::frame::FrameLoopShell{}.requireDriver(*parts_->game);
  if (!driver.pastBootPrefix()) {
    return false;
  }
  return true;
}

bool TitleSession::hasPicture() const {
  return parts_->booted && parts_->game->gpu_vk.lastPresented().valid();
}

void TitleSession::refreshPicture() const {
  if (!parts_->booted) {
    return;
  }
  // Each ask reads the presented image back off the GPU, and a boot runs for thousands of steps
  // whose first ones are black anyway.
  constexpr std::uint64_t kProbeEverySteps = 12;
  if (steps_ != 0 && steps_ - contentCheckedAtStep_ < kProbeEverySteps) {
    return;
  }
  contentCheckedAtStep_ = steps_;
  // The probe holds the frame it read: asked again while the session runs, never once it is
  // paused, since a frozen panel's picture cannot change.
  contentSeen_ = parts_->game->gpu_vk.retainFilledPresentImage();
}

int TitleSession::pictureWidth() const {
  const GpuVkState::PresentedImage image = presentedPicture();
  return pictureSize(image).width;
}

int TitleSession::pictureHeight() const {
  const GpuVkState::PresentedImage image = presentedPicture();
  return pictureSize(image).height;
}

// The same rect the compositor samples, or the panel crops to one aspect and draws another.
TitleSession::PaneSize TitleSession::pictureSize(const GpuVkState::PresentedImage &image) const {
  if (!image.valid()) {
    return PaneSize{1, 1};
  }
  const PaneRect picture = image.content.w > 0 && image.content.h > 0 ? image.content : image.viewport;
  return PaneSize{picture.w, picture.h};
}

// The held frame, because that is what the compositor samples.
GpuVkState::PresentedImage TitleSession::presentedPicture() const {
  if (!parts_->booted) {
    return {};
  }
  GpuVkState::PresentedImage image = parts_->game->gpu_vk.lastFilledPresented();
  if (!image.valid()) {
    image = parts_->game->gpu_vk.lastPresented();
  }
  return image;
}

void TitleSession::applyPresentRoute() {
  if (!parts_->game) {
    return; // no machine yet; boot() applies it
  }
  if (paneDestination_) {
    gpu_vk_present_to_pane(&parts_->game->core, paneImageW_, paneImageH_);
  } else {
    gpu_vk_present_to_window(&parts_->game->core);
  }
}

void TitleSession::presentToPane(int imageWidth, int imageHeight) {
  paneDestination_ = true;
  paneImageW_ = imageWidth;
  paneImageH_ = imageHeight;
  applyPresentRoute();
}

void TitleSession::presentToWindow() {
  paneDestination_ = false;
  paneImageW_ = 0;
  paneImageH_ = 0;
  applyPresentRoute();
}

void TitleSession::setPlayerInput(bool live) {
  playerInput_ = live;
  if (parts_->booted) {
    parts_->game->pad.setPlayerInputSuppressed(!live);
  }
}

void TitleSession::pressOnce(std::uint16_t activeLowMask, int frames) {
  if (!parts_->booted || activeLowMask == 0xFFFFu) {
    return;
  }
  parts_->game->pad.driveTap(activeLowMask, frames);
}

void TitleSession::pause() {
  if (paused_ || !parts_->booted) {
    paused_ = true;
    return;
  }
  paused_ = true;
  // A paused session presents nothing, so leaving its alarm armed would trip on a pause the
  // player chose.
  watchdog_disable();
}

void TitleSession::resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  if (parts_->booted) {
    watchdog_init();
  }
}

void TitleSession::setAudible(bool audible) {
  audible_ = audible;
  if (parts_->booted) {
    parts_->game->spu_audio.setOutputEnabled(audible);
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
