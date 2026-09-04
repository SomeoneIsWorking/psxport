#pragma once

#include "execution_exit.h"

#include <optional>

class Core;

namespace psx::cpu {

class ExecutionControl {
public:
  bool request(ExecutionResult result);
  std::optional<ExecutionResult> consume();
  const std::optional<ExecutionResult> &pending() const;

private:
  std::optional<ExecutionResult> pending_;
};

void requestExecutionExit(Core &core, ExecutionExitReason reason);
void requestExecutionExit(Core &core, ExecutionResult result);
bool completeOrPropagate(Core &core, ExecutionResult result);

} // namespace psx::cpu
