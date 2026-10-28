#include "native_dispatch.h"

#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "guest_memory_map.h"
#include "lightrec_executor.h"
#include "override_differential.h"
#include "platform_hle.h"
#include "side_effect_journal.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>
#include <optional>

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

NativeDispatcher::~NativeDispatcher() = default;

void NativeDispatcher::attachDifferential(std::unique_ptr<OverrideDifferential> differential) {
  differential_ = std::move(differential);
}

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
  const auto [entry, inserted] = entries_.try_emplace(
      registration.key, Entry{std::string(registration.name), registration.function, registration.producer});
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
  ++revision_;
  if (registration.producer) {
    ++producerAddresses_[registration.key.address];
    ++producerRevision_;
  }
  const std::uint32_t pc = physical(registration.key.address);
  core_.lightrecExecutor().invalidate({pc, pc + 4u});
  return true;
}

bool NativeDispatcher::remove(NativeKey key) {
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return false;
  }
  if (entry->second.producer) {
    const auto producer = producerAddresses_.find(key.address);
    if (--producer->second == 0) {
      producerAddresses_.erase(producer);
    }
    ++producerRevision_;
  }
  entries_.erase(entry);
  ++revision_;
  const std::uint32_t pc = physical(key.address);
  core_.lightrecExecutor().invalidate({pc, pc + 4u});
  return true;
}

bool NativeDispatcher::suppressed(NativeKey key) const {
  return std::find(suppressions_.begin(), suppressions_.end(), key) != suppressions_.end();
}

void NativeDispatcher::pushSuppression(NativeKey key) {
  suppressions_.push_back(key);
  ++revision_;
}

void NativeDispatcher::popSuppression() {
  suppressions_.pop_back();
  ++revision_;
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

  // The leaf's r[31] at entry: core.pc is the entry while the body runs, never a resume point.
  [[nodiscard]] std::uint32_t continuation() const {
    return continuation_;
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
  std::optional<Hle::PadWorkAreaAction> padWorkAreaAction;
  char biosTable = 0;
};

// A title's image-scoped override is consulted before the platform HLE table, which only answers for
// addresses no title claims.
ResolvedHostDispatch resolveHostDispatch(Core &core, std::uint32_t guestAddress) {
  const std::uint32_t target = physical(guestAddress);
  const bool isRam = target != 0;

  if (isRam) {
    if (const auto identity = core.imageCatalog().resolve(guestAddress)) {
      const NativeKey key{*identity, guestAddress};
      if (core.nativeDispatcher().intercepts(key)) {
        return {.kind = GuestHostDispatchKind::HostService, .nativeKey = key};
      }
    }
  }

  if (core.game) {
    if (NativeFunction service = core.game->platform_hle.lookup(guestAddress)) {
      return {.kind = GuestHostDispatchKind::HostService, .platformFunction = service};
    }
    const char biosTable = target == 0xa0u ? 'A' : target == 0xb0u ? 'B' : target == 0xc0u ? 'C' : '\0';
    if (biosTable) {
      return {.kind = GuestHostDispatchKind::HostService, .biosTable = biosTable};
    }
    if (auto action = core.game->hle.padWorkAreaAction(guestAddress)) {
      return {.kind = GuestHostDispatchKind::HostService, .padWorkAreaAction = action};
    }
  }
  if (!isRam) {
    return {.kind = GuestHostDispatchKind::HostService};
  }
  if (!core.imageCatalog().resolve(guestAddress)) {
    return {.kind = GuestHostDispatchKind::Fault};
  }
  return {};
}

// The journal's name for a resolved host service; only a title override is replayable.
SideEffectKind journaledKind(const ResolvedHostDispatch &resolved) {
  if (resolved.nativeKey.image.id != 0) {
    return SideEffectKind::NestedOverride;
  }
  if (resolved.biosTable != 0) {
    return SideEffectKind::BiosCall;
  }
  if (resolved.padWorkAreaAction) {
    return SideEffectKind::PadWorkArea;
  }
  return SideEffectKind::PlatformService;
}

std::uint32_t journaledSelector(const Core &core, const ResolvedHostDispatch &resolved) {
  return resolved.biosTable != 0 ? (core.r[9] & 0xffu) : 0u;
}

} // namespace

// How a host leaf the guest reached by jal finishes: its requested exit, else a return to r[31].
static ExecutionResult completeHostLeaf(Core &core, NativeExecutionScope &execution) {
  const std::optional<ExecutionResult> &pending = core.executionControl().pending();
  const bool unstated = pending.has_value() && pending->guestPc == 0u;
  if (auto requested = core.executionControl().consume(execution.continuation(), 0)) {
    // An unstated pc becomes the continuation and core.pc follows it; a stated one is its owner's.
    if (unstated) {
      execution.completeReturn();
    }
    return *requested;
  }
  execution.completeReturn();
  return {ExecutionExitReason::GuestReturn, core.pc, 0};
}

ExecutionResult invokeNativeFunction(Core &core, std::uint32_t guestAddress, NativeFunction function) {
  NativeExecutionScope execution(core, guestAddress);
  function(&core);
  return completeHostLeaf(core, execution);
}

GuestHostDispatchKind NativeDispatcher::classify(std::uint32_t guestAddress) {
  // The pad work-area entries answer from live guest memory, so they are never cached.
  if (Hle::isPadWorkAreaEntry(guestAddress)) {
    return resolveHostDispatch(core_, guestAddress).kind;
  }
  verdicts_.observe({.images = core_.imageCatalog().revision(),
                     .overrides = revision_,
                     .services = core_.game ? core_.game->platform_hle.revision() : 0u,
                     .game = core_.game});
  if (const auto cached = verdicts_.find(guestAddress)) {
    return *cached;
  }
  const GuestHostDispatchKind kind = resolveHostDispatch(core_, guestAddress).kind;
  verdicts_.store(guestAddress, kind);
  return kind;
}

ExecutionResult dispatchGuestHostService(Core &core, std::uint32_t guestAddress) {
  const ResolvedHostDispatch resolved = resolveHostDispatch(core, guestAddress);
  if (resolved.kind == GuestHostDispatchKind::Fault) {
    lucent::error("native-dispatch",
                  "guest address 0x{:08X} resolves to zero or multiple active code images; image identity "
                  "lookup: {}",
                  guestAddress,
                  core.imageCatalog().resolve(guestAddress).has_value() ? "CLAIMED by an active image"
                                                                        : "claimed by none");
    return {ExecutionExitReason::Fault, guestAddress, 0, "ambiguous code-image identity"};
  }
  if (resolved.kind != GuestHostDispatchKind::HostService) {
    return {ExecutionExitReason::Fault, guestAddress, 0, "guest address has no host service"};
  }
  if (core.sideEffectJournal != nullptr &&
      !core.sideEffectJournal->admitHostService(
          journaledKind(resolved), guestAddress, journaledSelector(core, resolved))) {
    // A shadow-path service the live path did not perform here is withheld from the devices.
    core.pc = core.r[31];
    return {ExecutionExitReason::GuestReturn, core.pc, 0};
  }
  if (resolved.platformFunction) {
    return invokeNativeFunction(core, guestAddress, resolved.platformFunction);
  }
  if (resolved.padWorkAreaAction) {
    NativeExecutionScope execution(core, guestAddress);
    core.game->hle.applyPadWorkAreaAction(*resolved.padWorkAreaAction);
    return completeHostLeaf(core, execution);
  }
  if (resolved.biosTable) {
    NativeExecutionScope execution(core, guestAddress);
    const std::uint32_t function = core.r[9] & 0xffu;
    if (core.game->hle.dispatchBios(resolved.biosTable, function)) {
      return completeHostLeaf(core, execution);
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
  return {ExecutionExitReason::GuestReturn, core.pc, 0};
}

ExecutionResult dispatchGuest(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view origin) {
  NativeCallerContextScope callerContext(core);
  const GuestHostDispatchKind kind = core.nativeDispatcher().classify(guestAddress);
  if (kind != GuestHostDispatchKind::ExecuteGuest) {
    // Named here: the executor's host-dispatch boundary reaches the same service.
    if (!core.imageCatalog().resolve(guestAddress).has_value()) {
      lucent::error("native-dispatch",
                    "dispatchGuest was handed 0x{:08X}, which is in no loaded code image, via the "
                    "entry classify path (not a host-dispatch boundary); caller: {}",
                    guestAddress,
                    origin);
    }
    return dispatchGuestHostService(core, guestAddress);
  }
  auto attribution = core.callAttribution.scope(guestAddress);
  return core.lightrecExecutor().executeFunction(guestAddress, core.r[31], budget);
}

ExecutionResult dispatchGuestUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  auto attribution = core.callAttribution.scope(guestAddress);
  return core.lightrecExecutor().executeUntilExit(guestAddress, budget);
}

bool NativeDispatcher::isProducer(std::uint32_t guestAddress) const {
  return producerAddresses_.contains(guestAddress);
}

std::optional<ExecutionResult> NativeDispatcher::invoke(NativeKey key) {
  const auto entry = entries_.find(key);
  if (entry == entries_.end() || suppressed(key)) {
    return std::nullopt;
  }
  std::optional<present::EmissionScope::Guard> emission;
  if (const std::optional<present::Producer> &producer = entry->second.producer) {
    const std::uint32_t object =
        producer->object == present::Arg::None ? 0u : core_.r[4 + static_cast<unsigned>(producer->object)];
    emission.emplace(core_.emission, key.address, object, 0u);
  }
  if (differential_) {
    if (auto shadowed = differential_->intercept(key, entry->second.name, entry->second.function)) {
      return shadowed;
    }
  }
  return invokeNativeFunction(core_, key.address, entry->second.function);
}

void dispatchGuestToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(dispatchGuest(core, guestAddress, budget, owner), owner)) {
    std::abort();
  }
}

namespace {

ExecutionResult executeOriginal(Core &core, NativeKey key, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  SuppressionScope suppression(core.nativeDispatcher(), key);
  return core.lightrecExecutor().executeFunction(key.address, core.r[31], budget);
}

} // namespace

ExecutionResult
resumeOriginal(Core &core, NativeKey key, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  SuppressionScope suppression(core.nativeDispatcher(), key);
  auto attribution = core.callAttribution.scope(key.address);
  return core.lightrecExecutor().executeFunction(resumePc, returnPc, budget);
}

ExecutionResult resumeGuestToReturn(Core &core, GuestCallSite call, std::uint32_t resumePc, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  auto attribution = core.callAttribution.scope(call.entry);
  return core.lightrecExecutor().executeFunction(resumePc, call.returnPc, budget);
}

ExecutionResult callOriginal(Core &core, NativeKey key, ExecutionBudget budget) {
  return executeOriginal(core, key, budget);
}

ExecutionResult callOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  const auto identity = core.imageCatalog().resolve(guestAddress);
  if (!identity) {
    return {ExecutionExitReason::Fault, guestAddress, 0, "ambiguous code-image identity"};
  }
  return executeOriginal(core, NativeKey{*identity, guestAddress}, budget);
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

bool callOriginalOrPropagate(Core &core, std::uint32_t guestAddress) {
  return completeOrPropagate(core, callOriginal(core, guestAddress, ExecutionBudget::currentTurn(core)));
}

bool dispatchGuestOrPropagate(Core &core, std::uint32_t guestAddress, std::string_view origin) {
  return completeOrPropagate(core, dispatchGuest(core, guestAddress, ExecutionBudget::currentTurn(core), origin));
}

std::optional<NativeKey> tryInstallNativeOverride(Core &core,
                                                  std::uint32_t guestAddress,
                                                  std::string_view name,
                                                  NativeFunction function,
                                                  std::optional<present::Producer> producer) {
  if (function == nullptr) {
    lucent::error(
        "native-dispatch", "cannot install '{}' at 0x{:08X}: it has no native implementation", name, guestAddress);
    return std::nullopt;
  }
  const std::optional<ImageIdentity> image = core.imageCatalog().resolve(guestAddress);
  if (!image) {
    lucent::error("native-dispatch",
                  "cannot install '{}' at 0x{:08X}: no active image owns that address, so it cannot "
                  "be attributed to whichever module happens to be resident",
                  name,
                  guestAddress);
    return std::nullopt;
  }
  const NativeKey key{*image, guestAddress};
  if (!core.nativeDispatcher().install(NativeRegistration{key, name, function, producer})) {
    return std::nullopt;
  }
  return key;
}

void installNativeOverride(Core &core,
                           std::uint32_t guestAddress,
                           std::string_view name,
                           NativeFunction function,
                           std::optional<present::Producer> producer) {
  if (!tryInstallNativeOverride(core, guestAddress, name, function, producer)) {
    std::abort();
  }
}

bool removeNativeOverride(Core &core, std::uint32_t guestAddress) {
  const std::optional<ImageIdentity> image = core.imageCatalog().resolve(guestAddress);
  if (!image) {
    lucent::error("native-dispatch",
                  "cannot remove the override at 0x{:08X}: no active image owns that address, so there is "
                  "no key to remove",
                  guestAddress);
    return false;
  }
  const NativeKey key{*image, guestAddress};
  if (!core.nativeDispatcher().remove(key)) {
    lucent::error("native-dispatch",
                  "cannot remove the override at 0x{:08X}: image {}:{} has no native owner there",
                  guestAddress,
                  key.image.id,
                  key.image.generation);
    return false;
  }
  return true;
}

} // namespace psx::cpu
