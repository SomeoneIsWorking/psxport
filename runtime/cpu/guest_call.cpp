#include "guest_call.h"

#include "core.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::cpu {

ExecutionResult dispatchGuestWithArguments(Core &core,
                                           std::uint32_t address,
                                           std::span<const std::uint32_t> arguments,
                                           ExecutionBudget budget) {
  if (arguments.size() > 4) {
    lucent::error("guest-call", "refused {} guest arguments; R3000 call ABI supports at most four", arguments.size());
    return {ExecutionExitReason::Fault, address, 0, "too many guest arguments"};
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    core.r[4 + index] = arguments[index];
  }
  return dispatchGuest(core, address, budget);
}

void dispatchGuestWithArgumentsToReturn(Core &core,
                                        std::uint32_t address,
                                        std::span<const std::uint32_t> arguments,
                                        ExecutionBudget budget,
                                        std::string_view owner) {
  if (!requireGuestReturn(dispatchGuestWithArguments(core, address, arguments, budget), owner)) {
    std::abort();
  }
}

} // namespace psx::cpu
