// resumable_guest_call — one guest call that may legitimately cross host turns.
//
// The contract, in six statements. Every one of them was previously re-derived by each title that
// owned a native function that calls guest code:
//
//   1. the return address is captured BEFORE the first dispatch and reused on every resume, so a
//      resume cannot adopt the nested `$ra` the guest body left behind and end the call elsewhere;
//   2. it is also latched into `r[31]`, because the guest entry returns through `jr $ra`;
//   3. every segment runs under `ExecutionBudget::currentTurn` - one display field - unless the
//      caller names another budget;
//   4. a segment that ends `BudgetExhausted` having consumed no cycles (or stopped at pc 0) made no
//      progress and is REFUSED, never retried;
//   5. the number of host turns a call may consume is a stated policy constant in display fields;
//   6. a stop is classified as Returned / Suspended / Refused, so a caller never reads a fault or a
//      yield as progress. A turn that ended BudgetExhausted, FrameBoundary or CooperativeYield is a
//      SUSPENSION: the first ran out of cycles, the second reached the guest's own field barrier, and
//      the third handed the turn back from a native replacement, and `CallStep::reason` says which.
//
// The ENTRY address, the RETURN address and the turn cap VALUE are the caller's facts. The loop,
// the latch, the refusals and the classification are not.
#pragma once

#include "execution_exit.h"
#include "image_identity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class Core;

namespace psx::cpu {

// A turn cap that states no cap: the caller (a title's frame driver) is the thing bounding the call.
inline constexpr std::uint32_t kUnboundedCallTurns = 0;

// Far above anything a finite guest call measures, and reported per call so it is falsifiable from
// the log rather than trusted.
inline constexpr std::uint32_t kDefaultCallTurns = 8u;

enum class CallOutcome : std::uint8_t {
  Returned,  // the call reached its return address; `value` is r[2]
  Suspended, // one host turn ran out; call `advance` again
  Refused,   // no progress, the turn cap, or a resume with no call pending: `detail` says which
};

struct CallStep {
  CallOutcome outcome = CallOutcome::Refused;
  std::uint32_t guestPc = 0; // where execution stopped; the next resume point when Suspended
  std::uint32_t turns = 0;   // display fields the call has consumed, this one included
  std::uint64_t cycles = 0;  // guest cycles over those turns
  std::uint32_t value = 0;   // r[2], once Returned
  std::string detail;        // the refusal, empty otherwise
  // WHY the turn ended, so a caller that counts display fields can count the ones that are display
  // fields. A title whose guest waits on VSync reaches FrameBoundary, and a title whose native
  // replacement yields a slice reaches CooperativeYield; neither is a budget exhaustion, and both are
  // a suspension rather than a fault.
  ExecutionExitReason reason = ExecutionExitReason::GuestReturn;
};

class ResumableGuestCall {
public:
  ResumableGuestCall() = default;

  // Captures the boundary and latches it into `r[31]`. `owner` names the native caller in every
  // report; `turnCap` is in display fields, and `kUnboundedCallTurns` says the caller bounds the
  // call itself. Refuses a second `begin` while a call is still pending.
  void begin(Core &core,
             std::string_view owner,
             std::uint32_t entry,
             std::uint32_t returnPc,
             std::uint32_t turnCap = kDefaultCallTurns);

  // Runs exactly one host turn and classifies how it stopped. `turn` defaults to one display field.
  // `original` names a native key whose override is suppressed for the whole call: the first segment
  // enters the original guest body and every resume re-establishes that suppression, so an original
  // cannot re-enter its own override.
  CallStep advance(const std::optional<NativeKey> &original = std::nullopt,
                   std::optional<ExecutionBudget> turn = std::nullopt);

  // Runs the call to its return address and returns its value. ABORTS on a refusal, naming the
  // owner, the entry, the return address, the turn and the reason: a caller that wanted a completed
  // guest call has none. Use `advance` when a refusal is something to report rather than to die on.
  std::uint32_t callToReturn(const std::optional<NativeKey> &original = std::nullopt);

  [[nodiscard]] bool pending() const {
    return pending_;
  }
  [[nodiscard]] std::uint32_t entry() const {
    return entry_;
  }
  [[nodiscard]] std::uint32_t returnPc() const {
    return returnPc_;
  }
  [[nodiscard]] std::uint32_t resumePc() const {
    return resumePc_;
  }
  [[nodiscard]] std::uint32_t turns() const {
    return turns_;
  }
  [[nodiscard]] std::uint64_t cycles() const {
    return cycles_;
  }

private:
  Core *core_ = nullptr;
  std::string owner_;
  std::uint32_t entry_ = 0;
  std::uint32_t returnPc_ = 0;
  std::uint32_t resumePc_ = 0;
  std::uint32_t turnCap_ = kDefaultCallTurns;
  std::uint32_t turns_ = 0;
  std::uint64_t cycles_ = 0;
  bool pending_ = false;
};

// The one-shot form for a native owner that cannot do anything else until its guest call returns:
// one display field per turn, capped by `turnCap`, aborting on a refusal. The entry/return facts and
// the cap are the caller's; the loop is not.
std::uint32_t callGuestToReturnResuming(Core &core,
                                        std::string_view owner,
                                        std::uint32_t entry,
                                        std::uint32_t returnPc,
                                        const std::optional<NativeKey> &original = std::nullopt,
                                        std::uint32_t turnCap = kDefaultCallTurns);

// The same, for the ORIGINAL guest body at `guestAddress`: the key is resolved through the active
// image identity (refused by name when no image owns it), the first segment runs with that key's
// override suppressed, and every resumed segment re-establishes that suppression, so guest code
// running inside the original cannot re-enter the override it is inside. This is the call an
// override makes when it needs its own guest body back.
std::uint32_t callOriginalResumingToReturn(Core &core,
                                           std::string_view owner,
                                           std::uint32_t guestAddress,
                                           std::uint32_t returnPc,
                                           std::uint32_t turnCap = kDefaultCallTurns);

} // namespace psx::cpu