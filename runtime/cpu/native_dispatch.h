#pragma once

#include "execution_exit.h"
#include "image_identity.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Core;

namespace psx::cpu {

class OverrideDifferential;
class SuppressionScope;

using NativeFunction = void (*)(Core *);

struct NativeRegistration {
  NativeKey key;
  std::string_view name;
  NativeFunction function = nullptr;
};

class NativeDispatcher {
public:
  explicit NativeDispatcher(Core &core);
  ~NativeDispatcher();
  NativeDispatcher(const NativeDispatcher &) = delete;
  NativeDispatcher &operator=(const NativeDispatcher &) = delete;

  bool install(NativeRegistration registration);
  bool remove(NativeKey key);
  std::optional<ExecutionResult> invoke(NativeKey key);
  bool isInstalled(NativeKey key) const;
  bool intercepts(NativeKey key) const;
  // Route every call of the overrides `differential` selects through it (override_differential.h).
  // Owned for the dispatcher's lifetime; its destructor writes the final report.
  void attachDifferential(std::unique_ptr<OverrideDifferential> differential);
  OverrideDifferential *differential() const;

private:
  struct NativeKeyHash {
    std::size_t operator()(NativeKey key) const;
  };
  struct Entry {
    std::string name;
    NativeFunction function = nullptr;
  };

  bool suppressed(NativeKey key) const;
  void pushSuppression(NativeKey key);
  void popSuppression();
  friend class SuppressionScope;

  Core &core_;
  std::unordered_map<NativeKey, Entry, NativeKeyHash> entries_;
  std::vector<NativeKey> suppressions_;
  std::unique_ptr<OverrideDifferential> differential_;
};

enum class GuestHostDispatchKind : std::uint8_t {
  ExecuteGuest,
  HostService,
  Fault,
};

GuestHostDispatchKind classifyGuestHostDispatch(Core &core, std::uint32_t guestAddress);
ExecutionResult dispatchGuestHostService(Core &core, std::uint32_t guestAddress);
ExecutionResult
invokeNativeFunction(Core &core, std::uint32_t guestAddress, NativeFunction function, std::string_view name);
// `origin` NAMES THE CALLER when `guestAddress` is not executable. Four call sites reach this
// function and a fault raised by the callee alone cannot say which supplied the address - measured
// on Mega Man X4, where the port's own boundary check did not fire and the address still arrived
// here, so the supplier was the framework rather than the title. The default keeps every existing
// caller compiling; each one passes its own name so the refusal is actionable.
ExecutionResult
dispatchGuest(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view origin = "unspecified");
ExecutionResult dispatchGuestUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
void dispatchGuestToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner);
// Resuming an original call that outlived one host turn. `ExecutionBudget::currentTurn` is one
// display field by construction, and the executor contract makes exceeding it an ORDINARY bounded
// exit that host code "commits and handles, then resumes deliberately" — so a guest function that
// needs more than one field is resumed, not aborted. Mega Man X4's `DecDCTvlc` needs 1.082 fields and
// terminates correctly on its own; Crash Bash's full-image channel swap is likewise finite compute.
//
// `resumePc` is where execution stopped (`ExecutionResult::guestPc` from the exhausted turn) and
// `returnPc` is the address that ends the call, normally the caller's `$ra`. On
// `ExecutionExitReason::GuestReturn` the call has completed; on `BudgetExhausted` the new
// `ExecutionResult::guestPc` is the next resume point and the caller must resume again. A resumed
// original re-establishes the native-suppression scope for `key`, so the override being executed
// cannot be re-entered from guest code running inside it; a dispatched guest call has no such scope.
ExecutionResult
resumeOriginal(Core &core, NativeKey key, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget);
ExecutionResult resumeGuestToReturn(Core &core, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget);
// The CORRECT form when the run reports OT submission attribution across a resume. Call attribution is per
// CALL, so it must be scoped on the call's entry — the address a fresh `dispatchGuest` would have used —
// and `resumePc` is MID-FUNCTION, so it cannot stand in for it. Without this, primitives submitted during a
// resumed turn are attributed to the enclosing frame instead of the call being resumed, which
// `runtime/psx/ot_attr.cpp` reports (`callAttribution.top()`, `caller()`, `visibleDepth()`). The
// entry-less form still works and still resumes correctly; it just cannot attribute. `resumeOriginal` needs
// no second form because its `NativeKey` already carries the entry.
ExecutionResult resumeGuestToReturnFrom(
    Core &core, std::uint32_t entry, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget);
ExecutionResult callOriginal(Core &core, NativeKey key, ExecutionBudget budget);
ExecutionResult callOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
ExecutionResult callOriginalUntilExit(Core &core, NativeKey key, ExecutionBudget budget);
ExecutionResult callOriginalUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
void callOriginalToReturn(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner);
void callOriginalToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner);
// Calls the original and KEEPS RESUMING it across host turns until it returns.
//
// WHY THIS EXISTS, and it is a contract contradiction rather than a convenience. `AGENTS.md` says budget
// exhaustion is "an ordinary bounded exit" that host code "commits and handles, then resumes
// deliberately", and the comment above `resumeOriginal` says the same in more words. But
// `callOriginalToReturn` cannot honour that: it returns `void`, so "exhausted, resume me at this PC"
// is inexpressible, and its only remaining move is `std::abort()`. A guest function that legitimately
// needs more than one host turn — Mega Man X4's `DecDCTvlc` needs 1.082 display fields, and a whole-image
// channel swap is more — therefore has exactly two options, both wrong: abort, or hand-roll this loop.
//
// **Three repositories hand-rolled it**, which is the duplication this exists to end. A loop that must
// be correct in three places at once is a loop the framework should own once.
//
// TWO DETAILS THAT ARE EASY TO GET WRONG AND ARE THEREFORE THIS FUNCTION'S JOB, not the caller's:
//
//   * `returnPc` is captured **before the first call**, from the caller's `$r[31]`. Capturing it after
//     would let a resume adopt whatever nested `$r[31]` the guest body left behind, which is a
//     different address and ends the call in the wrong place.
//   * The loop is **bounded**, and exceeding the bound is a loud named refusal, never a silent spin and
//     never an unbounded wait on a guest that will not return. A guest that needs more than this many
//     display fields is a guest loop, and saying so is more useful than hanging.
inline constexpr std::uint32_t kMaxResumedHostTurns = 64;
void callOriginalToReturnResuming(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner);
// The same bounded resume loop WITHOUT the refusal: returns `GuestReturn`, the first exit that is not a
// budget exhaustion, or the budget exhaustion at which `kMaxResumedHostTurns` was reached, with
// `cycles` accumulated over every turn. For a caller that must report an original's non-return rather
// than abort the process on its behalf (the override differential).
ExecutionResult callOriginalResumingToExit(Core &core, NativeKey key, ExecutionBudget budget);
void callOriginalToReturnResuming(Core &core,
                                  std::uint32_t guestAddress,
                                  ExecutionBudget budget,
                                  std::string_view owner);

} // namespace psx::cpu
