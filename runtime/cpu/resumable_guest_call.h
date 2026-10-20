// resumable_guest_call — one guest call that may cross host turns: the return address is captured
// before the first segment and latched into ra, each segment runs one display field, a turn that
// suspends keeps its register file for the next, and a stop is Returned, Suspended or Refused.
#pragma once

#include "execution_exit.h"
#include "guest_call_site.h"
#include "image_identity.h"
#include "r3000.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class Core;

namespace psx::cpu {

// A turn cap that states no cap: the caller (a title's frame driver) is the thing bounding the call.
inline constexpr std::uint32_t kUnboundedCallTurns = 0;

// Far above any finite guest call; each call's turns are reported.
inline constexpr std::uint32_t kDefaultCallTurns = 8u;

// An original run to its exit gets its first turn plus 64 resumes.
inline constexpr std::uint32_t kResumeToExitTurns = 65u;

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
  // Why the turn ended: BudgetExhausted, FrameBoundary and CooperativeYield all suspend.
  ExecutionExitReason reason = ExecutionExitReason::GuestReturn;
};

class ResumableGuestCall {
public:
  ResumableGuestCall() = default;

  // Captures the boundary and latches it into `r[31]`. `owner` names the native caller in every
  // report; `turnCap` is in display fields, and `kUnboundedCallTurns` says the caller bounds the
  // call itself. Refuses a second `begin` while a call is still pending.
  void begin(Core &core, std::string_view owner, GuestCallSite call, std::uint32_t turnCap = kDefaultCallTurns);

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
  R3000 suspended_{};
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

// The same for the original body at `guestAddress`, its override suppressed on every segment.
std::uint32_t callOriginalResumingToReturn(Core &core,
                                           std::string_view owner,
                                           std::uint32_t guestAddress,
                                           std::uint32_t returnPc,
                                           std::uint32_t turnCap = kDefaultCallTurns);

// The original body of `key` (ra as the return), resumed only across exhausted turns; returns its
// return or its first other exit with every turn's cycles, without reporting either.
ExecutionResult callOriginalResumingToExit(Core &core, std::string_view owner, NativeKey key);

} // namespace psx::cpu