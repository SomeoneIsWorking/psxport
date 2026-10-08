#include "execution_exit.h"

#include "core.h"

#include <lucent/log.h>

namespace psx::cpu {

ExecutionBudget ExecutionBudget::currentTurn(const Core &core) {
  // The host-turn deadline is not represented as a public Core duration yet. Keep the temporary
  // boundary finite and deterministic; the executor reports BudgetExhausted instead of extending it.
  // Replacing this with the timing owner's measured deadline is tracked at the executor integration
  // boundary, not duplicated by callers.
  (void)core;
  return fromCycles(33'868'800u / 60u);
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
