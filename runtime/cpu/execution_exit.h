#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class Core;

namespace psx::cpu {

enum class ExecutionExitReason : std::uint8_t {
  GuestReturn,
  BudgetExhausted,
  HostService,
  InterruptOrException,
  FrameBoundary,
  CooperativeYield,
  ProcessExit,
  Fault,
};

struct ExecutionBudget {
  std::uint64_t cycles = 0;

  static constexpr ExecutionBudget fromCycles(std::uint64_t value) {
    return {value};
  }
  static ExecutionBudget currentTurn(const Core &core);
};

struct ExecutionResult {
  ExecutionExitReason reason = ExecutionExitReason::Fault;
  std::uint32_t guestPc = 0;
  std::uint64_t cycles = 0;
  std::string detail{};

  constexpr bool returned() const {
    return reason == ExecutionExitReason::GuestReturn;
  }
};

const char *executionExitName(ExecutionExitReason reason);
bool requireGuestReturn(const ExecutionResult &result, std::string_view owner);

} // namespace psx::cpu
