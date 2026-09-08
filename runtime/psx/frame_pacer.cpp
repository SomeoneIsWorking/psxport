#include "frame_pacer.h"

#include "config.h"
#include "config_vars.h"
#include "core.h"
#include "field_rate.h"
#include "game.h"
#include "pace_plan.h"

#include <lucent/log.h>

#include <chrono>
#include <cstdlib>
#include <thread>

unsigned gpu_field_rate_millihz(Core *core) {
  return field_rate_millihz(core && core->game ? core->game->gpu.s_disp_pal != 0 : false);
}

PacePlan FramePacer::plan(PaceInputs inputs) {
  inputs.nextMs = nextMs_;
  inputs.seeded = seeded_;
  const PacePlan result = pace_plan(inputs);
  if (result.paced) {
    nextMs_ = result.nextMs;
    seeded_ = true;
  }
  return result;
}

namespace {

PacePlan preparePace(Core *core, int guestFields, int parts) {
  if (!core || !core->game) {
    lucent::error("pacer", "display-field pacing requires an owning Game instance");
    std::abort();
  }
  PaceInputs inputs;
  // NOPACE and resume fast-forward suppress host sleeping only. Neither changes windowing, the
  // guest display cadence, or the emulated time delivered below.
  inputs.unpaced = psx::config::cv_nopace.get() || (core && core->game && core->game->pad.fastForwarding());
  inputs.quota = guestFields > 0
                     ? guestFields
                     : ((core && core->cfg && core->cfg->paceQuota) ? static_cast<int>(core->cfg->paceQuota) : 0);
  inputs.parts = parts;
  inputs.fieldRateMilliHz = gpu_field_rate_millihz(core);
  inputs.nowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();

  const PacePlan plan = core->game->framePacer.plan(inputs);

  if (plan.quotaUnset) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      lucent::warn("gpu",
                   "GameConfig::paceQuota is unset — pacing 1 display field per call. Derive "
                   "this port's real cadence (fields per gpu_pace_frame call) and set it.");
    }
  }
  if (plan.rateUnset) {
    static bool warnedRate = false;
    if (!warnedRate) {
      warnedRate = true;
      lucent::warn("gpu",
                   "no display field rate — NOT pacing. GP1(08) has not been decoded for this "
                   "Core, so there is no clock to pace against and none will be invented.");
    }
  }

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

void gpu_pace_subframe_fields(Core *core, int guestFields, int parts) {
  const PacePlan plan = preparePace(core, guestFields, parts);
  if (!plan.rateUnset) {
    core->game->timing.advanceDisplayFields(plan.effectiveQuota, plan.effectiveParts, gpu_field_rate_millihz(core));
  }
  waitForPlan(plan, gpu_field_rate_millihz(core));
}

void gpu_wait_presented_fields(Core *core, int guestFields, int parts) {
  const PacePlan plan = preparePace(core, guestFields, parts);
  waitForPlan(plan, gpu_field_rate_millihz(core));
}

void gpu_pace_subframe(Core *core, int parts) {
  gpu_pace_subframe_fields(core, 0, parts);
}

void gpu_pace_frame(Core *core) {
  gpu_pace_subframe(core, 1);
}
