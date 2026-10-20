#pragma once

#include "core.h"
#include "guest_call_site.h"
#include "native_dispatch.h"
#include "r3000.h"

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

// The native-to-guest leaf call: it must return within the current host turn, `owner` names the caller
// in the failure report, and the arguments land in a0..a3. The one place the current-turn budget is spelled.
template <typename... Args>
  requires(sizeof...(Args) <= 4 && (std::convertible_to<Args, std::uint32_t> && ...))
void callGuestNow(Core &core, std::string_view owner, std::uint32_t address, Args... args) {
  const std::array<std::uint32_t, sizeof...(Args)> arguments{static_cast<std::uint32_t>(args)...};
  dispatchGuestWithArgumentsToReturn(core, address, arguments, ExecutionBudget::currentTurn(core), owner);
}

// callGuestNow for a jal site: ra is set to the site's return address first.
template <typename... Args>
  requires(sizeof...(Args) <= 4 && (std::convertible_to<Args, std::uint32_t> && ...))
void callGuestNow(Core &core, std::string_view owner, GuestCallSite call, Args... args) {
  core.r[kRa] = call.returnPc;
  callGuestNow(core, owner, call.entry, args...);
}

// The same current-turn call, returning how it exited for a caller that handles a non-return itself.
template <typename... Args>
  requires(sizeof...(Args) <= 4 && (std::convertible_to<Args, std::uint32_t> && ...))
ExecutionResult dispatchGuestNow(Core &core, std::uint32_t address, Args... args) {
  const std::array<std::uint32_t, sizeof...(Args)> arguments{static_cast<std::uint32_t>(args)...};
  return dispatchGuestWithArguments(core, address, arguments, ExecutionBudget::currentTurn(core));
}

// A current-turn run from `address` that stops only at an executor exit, for a title's frame turn.
inline ExecutionResult dispatchGuestUntilExitNow(Core &core, std::uint32_t address) {
  return dispatchGuestUntilExit(core, address, ExecutionBudget::currentTurn(core));
}

// callGuestNow for a leaf whose v0 the caller reads.
template <typename... Args>
  requires(sizeof...(Args) <= 4 && (std::convertible_to<Args, std::uint32_t> && ...))
std::uint32_t callGuestNowResult(Core &core, std::string_view owner, std::uint32_t address, Args... args) {
  callGuestNow(core, owner, address, args...);
  return guestReturnValue(core);
}

} // namespace psx::cpu
