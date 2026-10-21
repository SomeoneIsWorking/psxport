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

  // While one lives, a call cannot suspend at a turn's end, so each call gets `budget` in place of a turn.
  class Unsuspendable {
  public:
    Unsuspendable(ExecutionControl &control, ExecutionBudget budget);
    ~Unsuspendable();
    Unsuspendable(const Unsuspendable &) = delete;
    Unsuspendable &operator=(const Unsuspendable &) = delete;
    Unsuspendable(Unsuspendable &&) = delete;
    Unsuspendable &operator=(Unsuspendable &&) = delete;

  private:
    ExecutionControl &control_;
    std::optional<ExecutionBudget> outer_;
  };
  const std::optional<ExecutionBudget> &unsuspendable() const {
    return unsuspendable_;
  }

private:
  std::optional<ExecutionResult> pending_;
  std::optional<ExecutionBudget> unsuspendable_;
};

// Ask for a bounded exit. The reason-only form states no resume pc; its consumer supplies one
// (a jal'ed leaf's r[31], else the architectural pc), because a leaf's entry never is one.
void requestExecutionExit(Core &core, ExecutionExitReason reason);
void requestExecutionExit(Core &core, ExecutionResult result);
bool completeOrPropagate(Core &core, ExecutionResult result);

} // namespace psx::cpu
