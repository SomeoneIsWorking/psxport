#include "frame_pacer.h"

#include "core.h"
#include "field_rate.h"
#include "game.h"
#include "gpu_device.h"
#include "pace_plan.h"

#include <lucent/log.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace psx::frame {

unsigned FramePacer::fieldRateMilliHz(Core &core) const {
  return psx::frame::fieldRateMilliHz(core.gpuDevice().displayArea().pal);
}

PacePlan FramePacer::plan(PaceInputs inputs) {
  inputs.nextMs = nextMs_;
  inputs.seeded = seeded_;
  const PacePlan result = pacePlan(inputs);
  if (result.quotaUnset && !warnedQuotaUnset_) {
    warnedQuotaUnset_ = true;
    lucent::warn("gpu",
                 "no guest field count — pacing 1 display field per call. The title's frame driver "
                 "must pass its measured cadence (fields per pacing call).");
  }
  if (result.rateUnset && !warnedRateUnset_) {
    warnedRateUnset_ = true;
    lucent::warn("gpu",
                 "no display field rate — NOT pacing. GP1(08) has not been decoded for this "
                 "Core, so there is no clock to pace against and none will be invented.");
  }
  if (result.paced) {
    nextMs_ = result.nextMs;
    seeded_ = true;
  }
  return result;
}

namespace {

// `inputs` carries the driver's quota and parts; the rest is read here.
PacePlan preparePace(Core &core, PaceInputs inputs) {
  // NOPACE and a fast-forward (resume replay or host pre-roll) suppress host sleeping only. Neither changes windowing,
  // the guest display cadence, or the emulated time delivered below.
  inputs.unpaced = core.game->config.unpaced || core.game->fastForwarding();
  inputs.fieldRateMilliHz = core.game->framePacer.fieldRateMilliHz(core);
  inputs.nowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();

  const PacePlan plan = core.game->framePacer.plan(inputs);

  return plan;
}

void waitForPlan(const PacePlan &plan, unsigned fieldRateMilliHz) {
  if (!plan.paced) {
    return;
  }
  lucent::debug("pacer",
                "interval={:.4f}ms sleep={:.4f}ms quota={} parts={} rate={}mHz{}",
                plan.intervalMs,
                plan.sleepMs,
                plan.effectiveQuota,
                plan.effectiveParts,
                fieldRateMilliHz,
                plan.resync ? " RESYNC" : "");
  if (plan.sleepMs <= 0.0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(plan.sleepMs));
}

} // namespace

void FramePacer::paceSubframeFields(Core &core, FieldCadence cadence) {
  const PacePlan plan = preparePace(core, {.quota = cadence.fields, .parts = cadence.parts});
  if (!plan.rateUnset) {
    core.game->timing.advanceDisplayFields({plan.effectiveQuota, plan.effectiveParts}, fieldRateMilliHz(core));
  }
  waitForPlan(plan, fieldRateMilliHz(core));
}

void FramePacer::waitPresentedFields(Core &core, FieldCadence cadence) {
  const PacePlan plan = preparePace(core, {.quota = cadence.fields, .parts = cadence.parts});
  waitForPlan(plan, fieldRateMilliHz(core));
}

void FramePacer::hostScreenPace(Core &core) {
  PaceInputs inputs;
  inputs.unpaced = core.game->config.unpaced;
  inputs.quota = 1;
  inputs.parts = 1;
  inputs.fieldRateMilliHz = psx::frame::fieldRateMilliHz(false);
  inputs.nowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
  waitForPlan(core.game->framePacer.plan(inputs), inputs.fieldRateMilliHz);
}

} // namespace psx::frame
