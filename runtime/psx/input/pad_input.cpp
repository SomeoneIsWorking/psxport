// The PSX controller the guest reads: the effective active-low mask and the per-frame digital packet.
#include "pad_input.h"
#include "config_vars.h"
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "render_device.h"
#include <lucent/log.h>

void Pad::init() {
  buttons = psx::input::kNoButtons;
}

// A shown or unmapped window reads the same queue; forced and replayed masks are applied after this.
void Pad::pollHostInput() {
  buttons = mHost.poll(game->renderDevice.shown());
  // The debug keys act on the one Game the host gave the player's input.
  if (mPlayerInputSuppressed) {
    return;
  }
  // Control-channel taps last this Game's input turns, the pause loop's included, so a tap releases while paused.
  mHost.injection().afterInputTurn();
  if (mHost.takePauseRequest()) {
    game->dbg_server.togglePause();
  }
  if (mHost.takeFrameStepRequest()) {
    game->dbg_server.addStep(1);
  }
  if (mHost.takeBugReportRequest()) {
    game->bugReport.request();
  }
}

void Pad::setPlayerInputSuppressed(bool suppressed) {
  mPlayerInputSuppressed = suppressed;
  mHost.setOverlayLive(&game->rml_overlay, !suppressed);
}

void Pad::serviceFrame() {
  Core *c = &game->core;
  const bool shown = game->renderDevice.shown();
  pollHostInput();
  mHostButtons = buttons;

  if (!mForcedRead) {
    mForcedRead = true;
    mForced = psx::config::forced_input();
  }
  // The forced pulse is 8 frames down, 24 up, so each press is a fresh edge; past FORCE_HOLD_AT the hold wins.
  const bool stopped = mForced.stopAt >= 0 && static_cast<long>(mFc) >= mForced.stopAt;
  if ((mForced.pulse || mForced.hold) && !stopped) {
    if (mForced.hold && *mForced.hold != psx::input::kNoButtons && mFc >= mForced.holdAt) {
      buttons = *mForced.hold;
    } else {
      buttons = (mFc % 32u) < 8u ? mForced.pulse.value_or(psx::input::kNoButtons) : psx::input::kNoButtons;
    }
  }
  mFc++;

  // Before record/replay, so the guest, the recording and a replay agree.
  if (mPlayerInputSuppressed) {
    buttons = psx::input::kNoButtons;
  }

  if (!mSession.configured()) {
    mSession.configure(psx::input::PadSessionConfig{
        .recordPath = psx::config::pad_record_path(),
        .replayPath = psx::config::pad_replay_path(),
        .resumePath = psx::config::pad_resume_path(),
        .shown = shown,
        .liveInputOnly = mLiveInputOnly,
        .card =
            [this] {
              return game->memcard.snapshot();
            },
    });
  }
  buttons = mSession.service(c->runtime.inputPhase(*c), buttons);
  // Guest-pad taps count the frames the guest is served, so a paused or stepped run holds them.
  mHost.injection().afterPadFrame();
  // Edges latch only once every source has resolved to the mask the guest receives.
  mButtonEdges.sample(buttons);
  game->padProbes.service(*game, mSession.frameIndex());

  // A BIOS InitPAD2 user is served only while its PadCardIrq handler is enqueued and enabled.
  if (!game->hle.biosPadShouldService()) {
    return;
  }
  psx::input::PadDelivery delivery;
  delivery.packet = {0x00, 0x41, static_cast<uint8_t>(buttons & 0xFFu), static_cast<uint8_t>(buttons >> 8)};
  c->runtime.deliverPad(*c, delivery);
}
