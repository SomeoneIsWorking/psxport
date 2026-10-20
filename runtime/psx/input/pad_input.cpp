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

// A leg with no window keeps whatever forced or replayed mask it resolves; the drain still runs so a close ends the
// run.
void Pad::pollHostInput() {
  const bool windowed = game->renderDevice.windowed();
  const uint16_t hostMask = mHost.poll(windowed);
  if (!windowed) {
    return;
  }
  buttons = hostMask;
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

void Pad::driveHold(uint16_t activeLowMask) {
  repl_on = 1;
  repl_hold = activeLowMask;
}

void Pad::driveTap(Tap tap) {
  repl_on = 1;
  repl_tap = tap.activeLowMask;
  repl_tap_n = tap.frames;
}

// Relinquish the drive entirely, so no stale mask keeps overriding host or forced input.
void Pad::driveRelease() {
  repl_on = 0;
  repl_hold = psx::input::kNoButtons;
  repl_tap = psx::input::kNoButtons;
  repl_tap_n = 0;
}

void Pad::serviceFrame() {
  Core *c = &game->core;
  const bool windowed = game->renderDevice.windowed();
  pollHostInput();

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
  // A tap overrides the hold while it runs; a replay merges this mask rather than swallowing it.
  uint16_t repl_mask = psx::input::kNoButtons;
  if (repl_on) {
    if (repl_tap_n > 0) {
      repl_mask = repl_tap;
      repl_tap_n--;
    } else {
      repl_mask = repl_hold;
    }
    buttons = repl_mask;
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
        .windowed = windowed,
        .liveInputOnly = mLiveInputOnly,
        .card =
            [this] {
              return game->memcard.snapshot();
            },
    });
  }
  buttons = mSession.service(c->runtime.inputPhase(*c), {.live = buttons, .repl = repl_mask});
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
