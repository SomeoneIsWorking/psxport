#include "resumable_guest_call.h"

#include "blob_codec.h"
#include "core.h"
#include "guest_call.h"
#include "guest_call_census.h"
#include "native_dispatch.h"

#include <cstdlib>
#include <lucent/log.h>
#include <span>
#include <vector>

namespace psx::cpu {
namespace {

CallStep refused(std::string detail,
                 std::uint32_t guestPc,
                 std::uint32_t turns,
                 std::uint64_t cycles,
                 ExecutionExitReason reason = ExecutionExitReason::BudgetExhausted) {
  return CallStep{CallOutcome::Refused, guestPc, turns, cycles, 0u, std::move(detail), reason};
}

constexpr bool suspends(ExecutionExitReason reason) {
  return reason == ExecutionExitReason::BudgetExhausted || reason == ExecutionExitReason::FrameBoundary ||
         reason == ExecutionExitReason::CooperativeYield;
}

} // namespace

void ResumableGuestCall::begin(Core &core, std::string_view owner, GuestCallSite call, std::uint32_t turnCap) {
  const std::uint32_t entry = call.entry;
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
  if (!core.imageCatalog().resolve(entry)) {
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
  returnPc_ = call.returnPc;
  resumePc_ = entry;
  turnCap_ = turnCap;
  turns_ = 0;
  cycles_ = 0;
  pending_ = true;
  core.r[kRa] = call.returnPc;
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
    static_cast<R3000 &>(core) = suspended_;
  }
  ExecutionResult result =
      turns_ == 0u ? (original ? callOriginal(core, *original, budget) : dispatchGuest(core, entry_, budget, owner_))
                   : (original ? resumeOriginal(core, *original, resumePc_, returnPc_, budget)
                               : resumeGuestToReturn(core, GuestCallSite{entry_, returnPc_}, resumePc_, budget));
  cycles_ += result.cycles;
  ++turns_;

  if (result.returned()) {
    pending_ = false;
    core.guestCallCensus().recordCompleted(entry_, returnPc_, turns_, cycles_);
    return CallStep{CallOutcome::Returned, result.guestPc, turns_, cycles_, core.r[kV0], {}, result.reason};
  }
  if (!suspends(result.reason)) {
    return refused(lucent::format("the guest call exited as {}: {}", executionExitName(result.reason), result.detail),
                   result.guestPc,
                   turns_,
                   cycles_,
                   result.reason);
  }
  // Zero cycles is no progress only for an exhausted turn; a yield or a field barrier carries none by design.
  if (result.reason == ExecutionExitReason::BudgetExhausted && result.cycles == 0u) {
    return refused("the guest call exhausted a host turn having consumed no guest cycles",
                   result.guestPc,
                   turns_,
                   cycles_,
                   result.reason);
  }
  resumePc_ = result.guestPc;
  suspended_ = static_cast<const R3000 &>(core);
  return CallStep{CallOutcome::Suspended, result.guestPc, turns_, cycles_, 0u, {}, result.reason};
}

void ResumableGuestCall::save(psx::platform::BlobWriter &out) const {
  out.blob(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(owner_.data()), owner_.size()));
  out.u32(entry_);
  out.u32(returnPc_);
  out.u32(resumePc_);
  out.u32(turnCap_);
  out.u32(turns_);
  out.u64(cycles_);
  out.value(suspended_);
  out.boolean(pending_);
}

bool ResumableGuestCall::load(Core &core, psx::platform::BlobReader &in, std::string &error) {
  std::vector<std::uint8_t> owner;
  in.blob(owner);
  ResumableGuestCall call;
  call.core_ = &core;
  call.owner_.assign(owner.begin(), owner.end());
  call.entry_ = in.u32();
  call.returnPc_ = in.u32();
  call.resumePc_ = in.u32();
  call.turnCap_ = in.u32();
  call.turns_ = in.u32();
  call.cycles_ = in.u64();
  call.suspended_ = in.value<R3000>();
  call.pending_ = in.boolean();
  if (!in.decoded("guest call", error)) {
    return false;
  }
  *this = std::move(call);
  return true;
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
  call.begin(core, owner, GuestCallSite{entry, returnPc}, turnCap);
  return call.callToReturn(original);
}

std::uint32_t callOriginalResumingToReturn(
    Core &core, std::string_view owner, std::uint32_t guestAddress, std::uint32_t returnPc, std::uint32_t turnCap) {
  const std::optional<ImageIdentity> image = core.imageCatalog().resolve(guestAddress);
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

ExecutionResult callOriginalResumingToExit(Core &core, std::string_view owner, NativeKey key) {
  ResumableGuestCall call;
  call.begin(core, owner, GuestCallSite{key.address, core.r[kRa]}, kResumeToExitTurns);
  CallStep step = call.advance(key);
  while (step.outcome == CallOutcome::Suspended && step.reason == ExecutionExitReason::BudgetExhausted) {
    step = call.advance(key);
  }
  const ExecutionExitReason reason =
      step.outcome == CallOutcome::Returned ? ExecutionExitReason::GuestReturn : step.reason;
  return {reason, step.guestPc, step.cycles, std::move(step.detail)};
}

} // namespace psx::cpu
