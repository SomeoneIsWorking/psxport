#pragma once

#include "execution_exit.h"
#include "image_identity.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Core;

namespace psx::cpu {

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

  bool install(NativeRegistration registration);
  bool remove(NativeKey key);
  std::optional<ExecutionResult> invoke(NativeKey key);
  bool isInstalled(NativeKey key) const;
  bool intercepts(NativeKey key) const;

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
ExecutionResult dispatchGuest(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
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

} // namespace psx::cpu
