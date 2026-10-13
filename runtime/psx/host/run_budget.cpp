#include "run_budget.h"

#include "config_vars.h"
#include "dbg_server.h"
#include "gpu_vk.h"

#include <lucent/log.h>

namespace psx::host {

RunBudget RunBudget::decide(const RunBudgetInputs &inputs) {
  if (inputs.explicitCap > 0) {
    return RunBudget(static_cast<std::uint64_t>(inputs.explicitCap));
  }
  if (inputs.padRecording || inputs.repl || inputs.clientLive || inputs.windowed) {
    return RunBudget(0);
  }
  return RunBudget(kHeadlessDefaultSteps);
}

RunBudget RunBudget::fromConfig() {
  RunBudgetInputs inputs;
  inputs.explicitCap = psx::config::cv_native_frames.get();
  inputs.padRecording = !psx::config::cv_pad_replay.get().empty() || !psx::config::cv_pad_resume.get().empty();
  inputs.repl = psx::config::cv_repl.get();
  inputs.clientLive = debug_server_live();
  inputs.windowed = gpu_vk_windowed();
  const RunBudget budget = decide(inputs);
  lucent::info("run", "run budget: {} fence step(s)", budget.cap() != 0 ? std::to_string(budget.cap()) : "uncapped");
  return budget;
}

} // namespace psx::host
