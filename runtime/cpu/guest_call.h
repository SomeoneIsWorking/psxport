#pragma once

#include "native_dispatch.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>

class Core;

namespace psx::cpu {

ExecutionResult dispatchGuestWithArguments(Core &core,
                                           std::uint32_t address,
                                           std::span<const std::uint32_t> arguments,
                                           ExecutionBudget budget);

void dispatchGuestWithArgumentsToReturn(Core &core,
                                        std::uint32_t address,
                                        std::span<const std::uint32_t> arguments,
                                        ExecutionBudget budget,
                                        std::string_view owner);

inline ExecutionResult dispatchGuest0(Core &core, std::uint32_t address, ExecutionBudget budget) {
  return dispatchGuestWithArguments(core, address, {}, budget);
}

inline ExecutionResult dispatchGuest1(Core &core, std::uint32_t address, std::uint32_t a0, ExecutionBudget budget) {
  const std::array arguments{a0};
  return dispatchGuestWithArguments(core, address, arguments, budget);
}

inline ExecutionResult
dispatchGuest2(Core &core, std::uint32_t address, std::uint32_t a0, std::uint32_t a1, ExecutionBudget budget) {
  const std::array arguments{a0, a1};
  return dispatchGuestWithArguments(core, address, arguments, budget);
}

inline ExecutionResult dispatchGuest3(
    Core &core, std::uint32_t address, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, ExecutionBudget budget) {
  const std::array arguments{a0, a1, a2};
  return dispatchGuestWithArguments(core, address, arguments, budget);
}

inline ExecutionResult dispatchGuest4(Core &core,
                                      std::uint32_t address,
                                      std::uint32_t a0,
                                      std::uint32_t a1,
                                      std::uint32_t a2,
                                      std::uint32_t a3,
                                      ExecutionBudget budget) {
  const std::array arguments{a0, a1, a2, a3};
  return dispatchGuestWithArguments(core, address, arguments, budget);
}

inline void dispatchGuestToReturn0(Core &core, std::uint32_t address, ExecutionBudget budget, std::string_view owner) {
  dispatchGuestWithArgumentsToReturn(core, address, {}, budget, owner);
}

inline void dispatchGuestToReturn1(
    Core &core, std::uint32_t address, std::uint32_t a0, ExecutionBudget budget, std::string_view owner) {
  const std::array arguments{a0};
  dispatchGuestWithArgumentsToReturn(core, address, arguments, budget, owner);
}

inline void dispatchGuestToReturn2(Core &core,
                                   std::uint32_t address,
                                   std::uint32_t a0,
                                   std::uint32_t a1,
                                   ExecutionBudget budget,
                                   std::string_view owner) {
  const std::array arguments{a0, a1};
  dispatchGuestWithArgumentsToReturn(core, address, arguments, budget, owner);
}

inline void dispatchGuestToReturn3(Core &core,
                                   std::uint32_t address,
                                   std::uint32_t a0,
                                   std::uint32_t a1,
                                   std::uint32_t a2,
                                   ExecutionBudget budget,
                                   std::string_view owner) {
  const std::array arguments{a0, a1, a2};
  dispatchGuestWithArgumentsToReturn(core, address, arguments, budget, owner);
}

inline void dispatchGuestToReturn4(Core &core,
                                   std::uint32_t address,
                                   std::uint32_t a0,
                                   std::uint32_t a1,
                                   std::uint32_t a2,
                                   std::uint32_t a3,
                                   ExecutionBudget budget,
                                   std::string_view owner) {
  const std::array arguments{a0, a1, a2, a3};
  dispatchGuestWithArgumentsToReturn(core, address, arguments, budget, owner);
}

// The common native-to-guest leaf call: the guest function must return within the current host turn
// (ExecutionBudget::currentTurn), `owner` names the native caller for the failure report, and every
// argument lands in r4..r7 in order. This is the one home of that budget policy; callers do not spell
// the budget themselves.
template <typename... Args>
  requires(sizeof...(Args) <= 4 && (std::convertible_to<Args, std::uint32_t> && ...))
void callGuestNow(Core &core, std::string_view owner, std::uint32_t address, Args... args) {
  const std::array<std::uint32_t, sizeof...(Args)> arguments{static_cast<std::uint32_t>(args)...};
  dispatchGuestWithArgumentsToReturn(core, address, arguments, ExecutionBudget::currentTurn(core), owner);
}

} // namespace psx::cpu
