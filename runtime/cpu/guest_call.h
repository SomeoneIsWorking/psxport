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

// v0 after a guest call returned.
std::uint32_t guestReturnValue(const Core &core);

void dispatchGuestWithArgumentsToReturn(Core &core,
                                        std::uint32_t address,
                                        std::span<const std::uint32_t> arguments,
                                        ExecutionBudget budget,
                                        std::string_view owner);

inline ExecutionResult dispatchGuest0(Core &core, std::uint32_t address, ExecutionBudget budget) {
  return dispatchGuestWithArguments(core, address, {}, budget);
}

inline void dispatchGuestToReturn0(Core &core, std::uint32_t address, ExecutionBudget budget, std::string_view owner) {
  dispatchGuestWithArgumentsToReturn(core, address, {}, budget, owner);
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

// callGuestNow for a leaf whose v0 the caller reads.
template <typename... Args>
  requires(sizeof...(Args) <= 4 && (std::convertible_to<Args, std::uint32_t> && ...))
std::uint32_t callGuestNowResult(Core &core, std::string_view owner, std::uint32_t address, Args... args) {
  callGuestNow(core, owner, address, args...);
  return guestReturnValue(core);
}

} // namespace psx::cpu
