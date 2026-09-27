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

std::optional<ExecutionResult> ExecutionControl::consume() {
  auto result = std::move(pending_);
  pending_.reset();
  return result;
}

const std::optional<ExecutionResult> &ExecutionControl::pending() const {
  return pending_;
}

void requestExecutionExit(Core &core, ExecutionExitReason reason) {
  // guestPc 0 means "unstated", and the consumer supplies the correct continuation. See the contract
  // in execution_control.h: stamping `core.pc` here is wrong for every request raised inside a
  // `jal`ed native override, because that is the override's own entry.
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
