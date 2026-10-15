#include "run_budget.h"

#include "config_vars.h"
#include "dbg_server.h"
#include "typed_config.h"

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

RunBudget RunBudget::fromConfig(const psx::config::Config &config) {
  RunBudgetInputs inputs;
  inputs.explicitCap = psx::config::native_frames();
  inputs.padRecording = !psx::config::pad_replay_path().empty() || !psx::config::pad_resume_path().empty();
  inputs.repl = config.repl;
  inputs.clientLive = psx::debug::debugServerLive();
  inputs.windowed = psx::config::windowRequested();
  const RunBudget budget = decide(inputs);
  lucent::info("run", "run budget: {} fence step(s)", budget.cap() != 0 ? std::to_string(budget.cap()) : "uncapped");
  return budget;
}

} // namespace psx::host
