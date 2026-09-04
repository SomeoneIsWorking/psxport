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
  requestExecutionExit(core, ExecutionResult{reason, core.pc, 0, {}});
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
