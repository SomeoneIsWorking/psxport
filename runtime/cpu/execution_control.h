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

// Ask for a bounded exit. `reason`-only is the WEAK form: it states no resume address, and whoever
// consumes the request supplies the correct one for where the request was raised —
// `NativeExecutionScope` fills the leaf's `r[31]` continuation when the request came from a native
// override the guest reached by `jal`, and the executor fills the standing architectural PC when it
// came from interrupt or scheduler work. That split is not a convenience: a `jal`ed leaf's entry is
// never a valid resume point, so stamping it here and resuming there spins the leaf forever.
//
// Use the `result` overload to state a resume address explicitly, and it is always honoured.
void requestExecutionExit(Core &core, ExecutionExitReason reason);
void requestExecutionExit(Core &core, ExecutionResult result);
bool completeOrPropagate(Core &core, ExecutionResult result);

} // namespace psx::cpu
