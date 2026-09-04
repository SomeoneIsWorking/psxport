#include "native_dispatch.h"

#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "lightrec_executor.h"
#include "platform_hle.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>

namespace psx::cpu {

class SuppressionScope {
public:
  SuppressionScope(NativeDispatcher &dispatcher, NativeKey key) : dispatcher_(dispatcher) {
    dispatcher_.pushSuppression(key);
  }
  ~SuppressionScope() {
    dispatcher_.popSuppression();
  }

private:
  NativeDispatcher &dispatcher_;
};

NativeDispatcher::NativeDispatcher(Core &core) : core_(core) {}

std::size_t NativeDispatcher::NativeKeyHash::operator()(NativeKey key) const {
  const std::size_t a = static_cast<std::size_t>(key.image.id ^ (key.image.id >> 32));
  const std::size_t g = static_cast<std::size_t>(key.image.generation ^ (key.image.generation >> 32));
  return a ^ (g << 1) ^ (static_cast<std::size_t>(key.address) << 2);
}

bool NativeDispatcher::install(NativeRegistration registration) {
  if (!registration.key.image.id || !registration.key.image.generation || !registration.function ||
      registration.name.empty()) {
    lucent::error("native-dispatch", "refused incomplete native override registration");
    return false;
  }
  const auto [entry, inserted] =
      entries_.try_emplace(registration.key, Entry{std::string(registration.name), registration.function});
  if (!inserted) {
    lucent::error("native-dispatch",
                  "refused duplicate override '{}' at image {}:{} address 0x{:08X}; owned by '{}'",
                  registration.name,
                  registration.key.image.id,
                  registration.key.image.generation,
                  registration.key.address,
                  entry->second.name);
    return false;
  }
  core_.lightrecExecutor().invalidate(
      {registration.key.address & 0x1fffffffu, (registration.key.address & 0x1fffffffu) + 4u});
  return true;
}

bool NativeDispatcher::remove(NativeKey key) {
  if (!entries_.erase(key)) {
    return false;
  }
  core_.lightrecExecutor().invalidate({key.address & 0x1fffffffu, (key.address & 0x1fffffffu) + 4u});
  return true;
}

bool NativeDispatcher::suppressed(NativeKey key) const {
  return std::find(suppressions_.begin(), suppressions_.end(), key) != suppressions_.end();
}

void NativeDispatcher::pushSuppression(NativeKey key) {
  suppressions_.push_back(key);
}

void NativeDispatcher::popSuppression() {
  suppressions_.pop_back();
}

bool NativeDispatcher::isInstalled(NativeKey key) const {
  return entries_.find(key) != entries_.end();
}

bool NativeDispatcher::intercepts(NativeKey key) const {
  return isInstalled(key) && !suppressed(key);
}

namespace {

class NativeExecutionScope {
public:
  NativeExecutionScope(Core &core, std::uint32_t guestAddress)
      : core_(core), previousPc_(core.pc), previousActiveAddress_(core.active_native_address),
        continuation_(core.r[31]) {
    core_.active_native_address = guestAddress;
    core_.pc = guestAddress;
  }

  ~NativeExecutionScope() {
    core_.active_native_address = previousActiveAddress_;
    if (previousActiveAddress_ != 0) {
      core_.pc = previousPc_;
    }
  }

  void completeReturn() {
    core_.pc = continuation_;
  }

private:
  Core &core_;
  std::uint32_t previousPc_ = 0;
  std::uint32_t previousActiveAddress_ = 0;
  std::uint32_t continuation_ = 0;
};

class NativeCallerContextScope {
public:
  explicit NativeCallerContextScope(Core &core)
      : core_(core), previousPc_(core.pc), restore_(core.active_native_address != 0) {}

  ~NativeCallerContextScope() {
    if (restore_) {
      core_.pc = previousPc_;
    }
  }

private:
  Core &core_;
  std::uint32_t previousPc_ = 0;
  bool restore_ = false;
};

struct ResolvedHostDispatch {
  GuestHostDispatchKind kind = GuestHostDispatchKind::ExecuteGuest;
  NativeKey nativeKey{};
  NativeFunction platformFunction = nullptr;
  char biosTable = 0;
};

ResolvedHostDispatch resolveHostDispatch(Core &core, std::uint32_t guestAddress) {
  if (core.game) {
    if (NativeFunction service = core.game->platform_hle.lookup(guestAddress)) {
      return {.kind = GuestHostDispatchKind::HostService, .platformFunction = service};
    }
    const std::uint32_t physical = guestAddress & 0x1fffffffu;
    const char biosTable = physical == 0xa0u ? 'A' : physical == 0xb0u ? 'B' : physical == 0xc0u ? 'C' : 0;
    if (biosTable) {
      return {.kind = GuestHostDispatchKind::HostService, .biosTable = biosTable};
    }
  }
  if ((guestAddress & 0x1fffffffu) == 0) {
    return {.kind = GuestHostDispatchKind::HostService};
  }
  const auto identity = core.currentImageIdentity(guestAddress);
  if (!identity) {
    return {.kind = GuestHostDispatchKind::Fault};
  }
  const NativeKey key{*identity, guestAddress};
  if (core.nativeDispatcher().intercepts(key)) {
    return {.kind = GuestHostDispatchKind::HostService, .nativeKey = key};
  }
  return {};
}

} // namespace

ExecutionResult
invokeNativeFunction(Core &core, std::uint32_t guestAddress, NativeFunction function, std::string_view name) {
  NativeExecutionScope execution(core, guestAddress);
  function(&core);
  if (auto requested = core.executionControl().consume()) {
    return *requested;
  }
  execution.completeReturn();
  return {ExecutionExitReason::GuestReturn, core.pc, 0, std::string(name)};
}

GuestHostDispatchKind classifyGuestHostDispatch(Core &core, std::uint32_t guestAddress) {
  return resolveHostDispatch(core, guestAddress).kind;
}

ExecutionResult dispatchGuestHostService(Core &core, std::uint32_t guestAddress) {
  const ResolvedHostDispatch resolved = resolveHostDispatch(core, guestAddress);
  if (resolved.kind == GuestHostDispatchKind::Fault) {
    lucent::error(
        "native-dispatch", "guest address 0x{:08X} resolves to zero or multiple active code images", guestAddress);
    return {ExecutionExitReason::Fault, guestAddress, 0, "ambiguous code-image identity"};
  }
  if (resolved.kind != GuestHostDispatchKind::HostService) {
    return {ExecutionExitReason::Fault, guestAddress, 0, "guest address has no host service"};
  }
  if (resolved.platformFunction) {
    return invokeNativeFunction(core, guestAddress, resolved.platformFunction, "platform-hle");
  }
  if (resolved.biosTable) {
    NativeExecutionScope execution(core, guestAddress);
    const std::uint32_t function = core.r[9] & 0xffu;
    if (core.game->hle.dispatchBios(resolved.biosTable, function)) {
      execution.completeReturn();
      return {ExecutionExitReason::GuestReturn, core.pc, 0, "BIOS HLE"};
    }
    lucent::error("native-dispatch", "unimplemented BIOS {}0:0x{:02X}", resolved.biosTable, function);
    return {ExecutionExitReason::Fault, guestAddress, 0, "unimplemented BIOS service"};
  }
  if (resolved.nativeKey.image.id != 0) {
    auto attribution = core.callAttribution.scope(guestAddress);
    if (auto result = core.nativeDispatcher().invoke(resolved.nativeKey)) {
      return *result;
    }
    return {ExecutionExitReason::Fault, guestAddress, 0, "native override disappeared during dispatch"};
  }
  core.pc = core.r[31];
  return {ExecutionExitReason::GuestReturn, core.pc, 0, "null callback"};
}

ExecutionResult dispatchGuest(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  const GuestHostDispatchKind kind = classifyGuestHostDispatch(core, guestAddress);
  if (kind != GuestHostDispatchKind::ExecuteGuest) {
    return dispatchGuestHostService(core, guestAddress);
  }
  auto attribution = core.callAttribution.scope(guestAddress);
  return core.lightrecExecutor().executeFunction(guestAddress, core.r[31], budget);
}

std::optional<ExecutionResult> NativeDispatcher::invoke(NativeKey key) {
  const auto entry = entries_.find(key);
  if (entry == entries_.end() || suppressed(key)) {
    return std::nullopt;
  }
  return invokeNativeFunction(core_, key.address, entry->second.function, entry->second.name);
}

void dispatchGuestToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(dispatchGuest(core, guestAddress, budget), owner)) {
    std::abort();
  }
}

ExecutionResult callOriginal(Core &core, NativeKey key, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  SuppressionScope suppression(core.nativeDispatcher(), key);
  return core.lightrecExecutor().executeFunction(key.address, core.r[31], budget);
}

ExecutionResult callOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  const auto identity = core.currentImageIdentity(guestAddress);
  if (!identity) {
    return {ExecutionExitReason::Fault, guestAddress, 0, "ambiguous code-image identity"};
  }
  return callOriginal(core, NativeKey{*identity, guestAddress}, budget);
}

void callOriginalToReturn(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(callOriginal(core, key, budget), owner)) {
    std::abort();
  }
}

void callOriginalToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(callOriginal(core, guestAddress, budget), owner)) {
    std::abort();
  }
}

} // namespace psx::cpu
