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
ExecutionResult callOriginal(Core &core, NativeKey key, ExecutionBudget budget);
ExecutionResult callOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
ExecutionResult callOriginalUntilExit(Core &core, NativeKey key, ExecutionBudget budget);
ExecutionResult callOriginalUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget);
void callOriginalToReturn(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner);
void callOriginalToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner);

} // namespace psx::cpu
