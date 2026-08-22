#include "frame_pacer.h"

#include "config.h"
#include "config_vars.h"
#include "core.h"
#include "field_rate.h"
#include "game.h"
#include "pace_plan.h"

#include <lucent/log.h>

#include <ctime>

unsigned gpu_field_rate_millihz(Core *core) {
  return field_rate_millihz(core && core->game ? core->game->gpu.s_disp_pal != 0 : false);
}

// The game loop runs unthrottled unless it is held to the game's own field interval. Cadence comes
// from the port's declared display-field quota and the standard programmed by the guest. This is
// called once per game frame, not from gpu_present, which boot paths may drive many times per frame.
void gpu_pace_subframe_fields(Core *core, int guestFields, int parts) {
  static double next = 0.0;
  static bool seeded = false;

  timespec timestamp{};
  clock_gettime(CLOCK_MONOTONIC, &timestamp);

  PaceInputs inputs;
  // NOPACE and resume fast-forward suppress host sleeping only. Neither changes windowing, the
  // guest display cadence, or the emulated time delivered below.
  inputs.unpaced = psx::config::cv_nopace.get() || (core && core->game && core->game->pad.fastForwarding());
  inputs.quota = guestFields > 0
                     ? guestFields
                     : ((core && core->cfg && core->cfg->paceQuota) ? static_cast<int>(core->cfg->paceQuota) : 0);
  inputs.parts = parts;
  inputs.fieldRateMilliHz = gpu_field_rate_millihz(core);
  inputs.nowMs = timestamp.tv_sec * 1000.0 + timestamp.tv_nsec / 1e6;
  inputs.nextMs = next;
  inputs.seeded = seeded;

  const PacePlan plan = pace_plan(inputs);

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

  if (core && core->game && !plan.rateUnset) {
    core->game->timing.advanceDisplayFields(plan.effectiveQuota, plan.effectiveParts, inputs.fieldRateMilliHz);
  }
  if (!plan.paced) {
    return;
  }

  next = plan.nextMs;
  seeded = true;
  lucent::debug("pacer",
                "interval={:.4f}ms sleep={:.4f}ms quota={} parts={} rate={}mHz{}",
                plan.intervalMs,
                plan.sleepMs,
                inputs.quota,
                parts,
                inputs.fieldRateMilliHz,
                plan.resync ? " RESYNC" : "");
  if (plan.sleepMs <= 0.0) {
    return;
  }
  const auto seconds = static_cast<time_t>(plan.sleepMs / 1000.0);
  const auto nanoseconds = static_cast<long>((plan.sleepMs - static_cast<long>(seconds) * 1000.0) * 1e6);
  const timespec request = {seconds, nanoseconds};
  nanosleep(&request, nullptr);
}

void gpu_pace_subframe(Core *core, int parts) {
  gpu_pace_subframe_fields(core, 0, parts);
}

void gpu_pace_frame(Core *core) {
  gpu_pace_subframe(core, 1);
}
