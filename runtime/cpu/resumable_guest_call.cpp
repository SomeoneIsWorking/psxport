#include "resumable_guest_call.h"

#include "core.h"
#include "guest_call.h"
#include "guest_call_census.h"
#include "native_dispatch.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::cpu {
namespace {

CallStep refused(std::string detail,
                 std::uint32_t guestPc,
                 std::uint32_t turns,
                 std::uint64_t cycles,
                 ExecutionExitReason reason = ExecutionExitReason::BudgetExhausted) {
  return CallStep{CallOutcome::Refused, guestPc, turns, cycles, 0u, std::move(detail), reason};
}

// A turn can end for a reason that is not a fault and not an exhaustion: the guest reached its own
// display-field barrier, or a native replacement finished a slice and handed the turn back. Both mean
// the host owes its caller a field, so both are suspensions. Everything else still has to be a return
// or a refusal.
constexpr bool suspends(ExecutionExitReason reason) {
  return reason == ExecutionExitReason::BudgetExhausted || reason == ExecutionExitReason::FrameBoundary ||
         reason == ExecutionExitReason::CooperativeYield;
}

} // namespace

void ResumableGuestCall::begin(
    Core &core, std::string_view owner, std::uint32_t entry, std::uint32_t returnPc, std::uint32_t turnCap) {
  if (pending_) {
    lucent::error("guest-call",
                  "{} began a second call at 0x{:08X} while 0x{:08X} is still suspended at 0x{:08X}",
                  owner,
                  entry,
                  entry_,
                  resumePc_);
    std::abort();
  }
  core_ = &core;
  if (!core.currentImageIdentity(entry)) {
    lucent::error("guest-call",
                  "{}: guest entry 0x{:08X} is in NO loaded code image, so it is not a guest address "
                  "at all: it is a value this owner passed through unexamined. Refused here, where "
                  "the owner can still be named, rather than at the dispatcher",
                  owner,
                  entry);
    std::abort();
  }
  owner_ = std::string{owner};
  entry_ = entry;
  returnPc_ = returnPc;
  resumePc_ = entry;
  turnCap_ = turnCap;
  turns_ = 0;
  cycles_ = 0;
  pending_ = true;
  // The boundary is the guest entry's `jr $ra`, so it is latched here rather than left to whatever
  // the guest last stored.
  core.r[31] = returnPc;
}

CallStep ResumableGuestCall::advance(const std::optional<NativeKey> &original, std::optional<ExecutionBudget> turn) {
  if (core_ == nullptr || !pending_) {
    return refused("resumed a guest call that is not pending", 0u, turns_, cycles_);
  }
  Core &core = *core_;
  const ExecutionBudget budget = turn ? *turn : ExecutionBudget::currentTurn(core);
  if (turnCap_ != kUnboundedCallTurns && turns_ >= turnCap_) {
    return refused("the call reached its turn cap", resumePc_, turns_, cycles_);
  }

  if (turns_ != 0u) {
    *static_cast<R3000 *>(&core) = suspended_;
  }
  ExecutionResult result =
      turns_ == 0u ? (original ? callOriginal(core, *original, budget) : dispatchGuest(core, entry_, budget, owner_))
                   : (original ? resumeOriginal(core, *original, resumePc_, returnPc_, budget)
                               : resumeGuestToReturn(core, resumePc_, returnPc_, budget));
  cycles_ += result.cycles;
  ++turns_;

  if (result.returned()) {
    pending_ = false;
    core.guestCallCensus().recordCompleted(entry_, returnPc_, turns_, cycles_);
    return CallStep{CallOutcome::Returned, result.guestPc, turns_, cycles_, core.r[2], {}, result.reason};
  }
  if (!suspends(result.reason)) {
    requireGuestReturn(result, owner_);
    return refused(std::string{"the guest call exited as "} + executionExitName(result.reason),
                   result.guestPc,
                   turns_,
                   cycles_,
                   result.reason);
  }
  // "No cycles" is only evidence of no progress for a turn that ran out of budget. A
  // `CooperativeYield` is a native replacement reporting the work it just did in HOST code - an FMV
  // frame is decoded, uploaded and drawn without the guest ever executing an instruction - so it
  // arrives carrying zero guest cycles by construction, and refusing it would kill every movie. A
  // `FrameBoundary` is the guest asking for a field, which is progress the host is about to deliver.
  //
  // Every suspending exit must state WHERE to resume, and that is already the framework's rule rather
  // than this class's: a request with no address of its own is handed back with the standing
  // architectural PC (lightrec_executor.cpp), so there is nothing to check here.
  if (result.reason == ExecutionExitReason::BudgetExhausted && result.cycles == 0u) {
    return refused("the guest call exhausted a host turn having consumed no guest cycles",
                   result.guestPc,
                   turns_,
                   cycles_,
                   result.reason);
  }
  resumePc_ = result.guestPc;
  suspended_ = *static_cast<const R3000 *>(&core);
  return CallStep{CallOutcome::Suspended, result.guestPc, turns_, cycles_, 0u, {}, result.reason};
}

std::uint32_t ResumableGuestCall::callToReturn(const std::optional<NativeKey> &original) {
  for (;;) {
    const CallStep step = advance(original);
    switch (step.outcome) {
    case CallOutcome::Returned:
      return step.value;
    case CallOutcome::Suspended:
      break;
    case CallOutcome::Refused:
      lucent::error("guest-call",
                    "{}: guest call 0x{:08X} to return address 0x{:08X} refused after {} host turn(s) and "
                    "{} cycles, stopped at 0x{:08X}: {}",
                    owner_,
                    entry_,
                    returnPc_,
                    step.turns,
                    step.cycles,
                    step.guestPc,
                    step.detail);
      std::abort();
    }
  }
}

std::uint32_t callGuestToReturnResuming(Core &core,
                                        std::string_view owner,
                                        std::uint32_t entry,
                                        std::uint32_t returnPc,
                                        const std::optional<NativeKey> &original,
                                        std::uint32_t turnCap) {
  ResumableGuestCall call;
  call.begin(core, owner, entry, returnPc, turnCap);
  return call.callToReturn(original);
}

std::uint32_t callOriginalResumingToReturn(
    Core &core, std::string_view owner, std::uint32_t guestAddress, std::uint32_t returnPc, std::uint32_t turnCap) {
  const std::optional<ImageIdentity> image = core.currentImageIdentity(guestAddress);
  if (!image) {
    lucent::error("guest-call",
                  "{}: the original guest body at 0x{:08X} is in NO loaded code image, so there is no "
                  "key to suppress and no identity to attribute it to",
                  owner,
                  guestAddress);
    std::abort();
  }
  return callGuestToReturnResuming(core, owner, guestAddress, returnPc, NativeKey{*image, guestAddress}, turnCap);
}

} // namespace psx::cpu