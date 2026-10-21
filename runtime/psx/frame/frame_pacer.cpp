#include "frame_pacer.h"

#include "core.h"
#include "field_rate.h"
#include "game.h"
#include "host_clock.h"

#include <lucent/log.h>

#include <optional>

namespace psx::frame {

PacePlan FramePacer::plan(PaceInputs inputs) {
  inputs.nextMs = nextMs_;
  inputs.seeded = seeded_;
  const PacePlan result = pacePlan(inputs);
  if (result.quotaUnset && !warnedQuotaUnset_) {
    warnedQuotaUnset_ = true;
    lucent::warn(
        "gpu", "no guest field count: pacing 1 display field per call; the title's frame driver must pass its cadence");
  }
  if (result.rateUnset && !warnedRateUnset_) {
    warnedRateUnset_ = true;
    lucent::warn("gpu", "no display field rate: not pacing");
  }
  if (result.paced) {
    nextMs_ = result.nextMs;
    seeded_ = true;
  }
  return result;
}

std::optional<double> FramePacer::deadline(Core &core, PaceInputs inputs, bool advanceClock) {
  Game &game = *core.game;
  // Every earlier scheduled present reaches the screen before the next deadline is planned.
  game.display.finishShow();
  inputs.nowMs = psx::platform::hostNowMs();
  const PacePlan planned = plan(inputs);
  if (advanceClock && !planned.rateUnset) {
    game.timing.advanceDisplayFields({planned.effectiveQuota, planned.effectiveParts}, inputs.fieldRateMilliHz);
  }
  if (planned.paced) {
    pacedFields_ += static_cast<unsigned long long>(planned.effectiveQuota);
  }
  // The play check judges the next present against the fields this plan holds it for, paced or not.
  game.perf.heldFor(static_cast<double>(planned.effectiveQuota) / static_cast<double>(planned.effectiveParts));
  if (!planned.paced) {
    return std::nullopt;
  }
  lucent::debug("pacer",
                "interval={:.4f}ms sleep={:.4f}ms quota={} parts={} rate={}mHz{}",
                planned.intervalMs,
                planned.sleepMs,
                planned.effectiveQuota,
                planned.effectiveParts,
                inputs.fieldRateMilliHz,
                planned.resync ? " RESYNC" : "");
  return planned.nextMs;
}

void FramePacer::wait(std::optional<double> dueMs) {
  if (dueMs) {
    psx::platform::sleepUntilMs(*dueMs);
  }
}

namespace {

// A guest cadence at the standard the guest programmed; NOPACE and a fast-forward suppress the sleep
// only, never the emulated time delivered.
PaceInputs guestInputs(const Game &game, FieldCadence cadence) {
  return {.unpaced = game.config.unpaced || game.fastForwarding(),
          .quota = cadence.fields,
          .parts = cadence.parts,
          .fieldRateMilliHz = fieldRateMilliHz(game.timing.displayFieldRate())};
}

} // namespace

std::optional<double> FramePacer::subframeDeadline(Core &core, FieldCadence cadence) {
  return deadline(core, guestInputs(*core.game, cadence), true);
}

std::optional<double> FramePacer::presentedFieldsDeadline(Core &core, FieldCadence cadence) {
  return deadline(core, guestInputs(*core.game, cadence), false);
}

void FramePacer::paceSubframeFields(Core &core, FieldCadence cadence) {
  wait(subframeDeadline(core, cadence));
}

void FramePacer::waitPresentedFields(Core &core, FieldCadence cadence) {
  wait(presentedFieldsDeadline(core, cadence));
}

void FramePacer::hostScreenPace(Core &core) {
  wait(deadline(
      core,
      {.unpaced = core.game->config.unpaced, .quota = 1, .parts = 1, .fieldRateMilliHz = kHostScreenFieldRateMilliHz},
      false));
}

} // namespace psx::frame
