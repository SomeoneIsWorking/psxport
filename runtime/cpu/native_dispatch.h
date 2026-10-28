#pragma once

#include "emission_scope.h"
#include "execution_exit.h"
#include "guest_call_site.h"
#include "guest_host_dispatch_kind.h"
#include "host_dispatch_cache.h"
#include "image_identity.h"
#include "native_function.h"

#include <cstdint>
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

struct NativeRegistration {
  NativeKey key;
  std::string_view name;
  NativeFunction function = nullptr;
  // Set for a producer: the dispatcher opens the emission scope (address, that register) around the call.
  std::optional<present::Producer> producer;
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
  // What `guestAddress` is when execution reaches it, cached until an input of the answer changes;
  // an answer that reads live guest memory is never cached.
  GuestHostDispatchKind classify(std::uint32_t guestAddress);
  // Advances on every install, removal, and change of the suppression stack.
  std::uint64_t revision() const {
    return revision_;
  }
  // Whether an installed override at `guestAddress` (in any image) is a producer.
  bool isProducer(std::uint32_t guestAddress) const;
  bool hasProducers() const {
    return !producerAddresses_.empty();
  }
  // Advances whenever the set of producer addresses changes.
  std::uint64_t producerRevision() const {
    return producerRevision_;
  }

private:
  struct NativeKeyHash {
    std::size_t operator()(NativeKey key) const;
  };
  struct Entry {
    std::string name;
    NativeFunction function = nullptr;
    std::optional<present::Producer> producer;
  };

  bool suppressed(NativeKey key) const;
  void pushSuppression(NativeKey key);
  void popSuppression();
  friend class SuppressionScope;

  Core &core_;
  std::unordered_map<NativeKey, Entry, NativeKeyHash> entries_;
  std::vector<NativeKey> suppressions_;
  std::uint64_t revision_ = 0;
  std::unordered_map<std::uint32_t, int> producerAddresses_; // guest address -> installed producer keys
  std::uint64_t producerRevision_ = 0;
  HostDispatchVerdictCache verdicts_;
  std::unique_ptr<OverrideDifferential> differential_;
};

// The one way an override is registered: the key's image identity is resolved here. Refuses a null
// handler, an address no active image owns, and a key that already has an owner.
std::optional<NativeKey> tryInstallNativeOverride(Core &core,
                                                  std::uint32_t guestAddress,
                                                  std::string_view name,
                                                  NativeFunction function,
                                                  std::optional<present::Producer> producer = std::nullopt);
// The same refusal, then an abort, for a boot-time override catalog.
void installNativeOverride(Core &core,
                           std::uint32_t guestAddress,
                           std::string_view name,
                           NativeFunction function,
                           std::optional<present::Producer> producer = std::nullopt);
// Removes the override the active image identity resolves at `guestAddress`; refuses an unowned address.
bool removeNativeOverride(Core &core, std::uint32_t guestAddress);
ExecutionResult dispatchGuestHostService(Core &core, std::uint32_t guestAddress);
ExecutionResult invokeNativeFunction(Core &core, std::uint32_t guestAddress, NativeFunction function);
// `origin` names the caller in the refusal when `guestAddress` is not executable.
ExecutionResult dispatchGuest(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view origin);
ExecutionResult dispatchGuestUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
void dispatchGuestToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner);
// Resumes a call that outlived one host turn at `resumePc`, ending at `returnPc`; the key's override
// stays suppressed, as it was for the first segment. ResumableGuestCall owns the loop.
ExecutionResult
resumeOriginal(Core &core, NativeKey key, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget);
// Call attribution is scoped on `call.entry`; `resumePc` is mid-function.
ExecutionResult resumeGuestToReturn(Core &core, GuestCallSite call, std::uint32_t resumePc, ExecutionBudget budget);
ExecutionResult callOriginal(Core &core, NativeKey key, ExecutionBudget budget);
ExecutionResult callOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
void callOriginalToReturn(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner);
void callOriginalToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner);
// The original body under the current-turn budget; a non-return is propagated as the caller's exit.
bool callOriginalOrPropagate(Core &core, std::uint32_t guestAddress);
// `dispatchGuest` under the current-turn budget, propagating a non-return the same way.
bool dispatchGuestOrPropagate(Core &core, std::uint32_t guestAddress, std::string_view origin);

} // namespace psx::cpu
