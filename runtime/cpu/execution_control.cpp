#include "execution_control.h"

#include "core.h"
#include "lightrec_executor.h"

#include <utility>

namespace psx::cpu {

bool ExecutionControl::request(ExecutionResult result) {
  if (pending_) {
    return false;
  }
  pending_ = std::move(result);
  return true;
}

std::optional<ExecutionResult> ExecutionControl::consume(std::uint32_t unstatedPc, std::uint64_t cycles) {
  std::optional<ExecutionResult> result = std::move(pending_);
  pending_.reset();
  if (result) {
    result->cycles += cycles;
    if (result->guestPc == 0u) {
      result->guestPc = unstatedPc;
    }
  }
  return result;
}

const std::optional<ExecutionResult> &ExecutionControl::pending() const {
  return pending_;
}

ExecutionControl::Unsuspendable::Unsuspendable(ExecutionControl &control, ExecutionBudget budget)
    : control_(control), outer_(control.unsuspendable_) {
  control_.unsuspendable_ = budget;
}

ExecutionControl::Unsuspendable::~Unsuspendable() {
  control_.unsuspendable_ = outer_;
}

void requestExecutionExit(Core &core, ExecutionExitReason reason) {
  requestExecutionExit(core, ExecutionResult{reason, 0, 0, {}});
}

void requestExecutionExit(Core &core, ExecutionResult result) {
  if (core.executionControl().request(std::move(result))) {
    core.lightrecExecutor().requestStop();
  }
}

bool completeOrPropagate(Core &core, ExecutionResult result) {
  if (result.returned()) {
    return true;
  }
  requestExecutionExit(core, std::move(result));
  return false;
}

} // namespace psx::cpu
