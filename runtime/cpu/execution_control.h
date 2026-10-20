#pragma once

#include "execution_exit.h"

#include <cstdint>
#include <optional>

class Core;

namespace psx::cpu {

class ExecutionControl {
public:
  bool request(ExecutionResult result);
  // Takes the pending request; an unstated resume pc (0) becomes `unstatedPc`, and `cycles` are added.
  std::optional<ExecutionResult> consume(std::uint32_t unstatedPc, std::uint64_t cycles);
  const std::optional<ExecutionResult> &pending() const;

private:
  std::optional<ExecutionResult> pending_;
};

// Ask for a bounded exit. The reason-only form states no resume pc; its consumer supplies one
// (a jal'ed leaf's r[31], else the architectural pc), because a leaf's entry never is one.
void requestExecutionExit(Core &core, ExecutionExitReason reason);
void requestExecutionExit(Core &core, ExecutionResult result);
bool completeOrPropagate(Core &core, ExecutionResult result);

} // namespace psx::cpu
