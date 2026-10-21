#include "execution_exit.h"

#include "core.h"
#include "emulated_time.h"
#include "execution_control.h"
#include "field_rate.h"
#include "game.h"

#include <lucent/log.h>

#include <optional>

namespace psx::cpu {

// One display field of guest cycles at the core's display rate, or the budget of a call that cannot suspend.
ExecutionBudget ExecutionBudget::currentTurn(const Core &core) {
  if (const std::optional<ExecutionBudget> &held = core.executionControl().unsuspendable()) {
    return *held;
  }
  const psx::frame::DisplayFieldRate rate =
      core.game != nullptr ? core.game->timing.displayFieldRate() : psx::frame::DISPLAY_FIELD_RATE_NTSC;
  return fromCycles(psx::frame::displayFieldCpuTicks(1, 1, psx::frame::fieldRateMilliHz(rate)));
}

const char *executionExitName(ExecutionExitReason reason) {
  switch (reason) {
  case ExecutionExitReason::GuestReturn:
    return "guest-return";
  case ExecutionExitReason::BudgetExhausted:
    return "budget-exhausted";
  case ExecutionExitReason::HostService:
    return "host-service";
  case ExecutionExitReason::InterruptOrException:
    return "interrupt-or-exception";
  case ExecutionExitReason::FrameBoundary:
    return "frame-boundary";
  case ExecutionExitReason::CooperativeYield:
    return "cooperative-yield";
  case ExecutionExitReason::ProcessExit:
    return "process-exit";
  case ExecutionExitReason::Fault:
    return "fault";
  }
  return "invalid";
}

bool requireGuestReturn(const ExecutionResult &result, std::string_view owner) {
  if (result.returned()) {
    return true;
  }
  lucent::error("executor",
                "{} required a completed guest call, but execution exited as {} at 0x{:08X} "
                "after {} cycles: {}",
                owner,
                executionExitName(result.reason),
                result.guestPc,
                result.cycles,
                result.detail);
  return false;
}

} // namespace psx::cpu
