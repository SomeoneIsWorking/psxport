#include "store_observe.h"

#include "config_vars.h"
#include "core.h"
#include "r3000.h"

#include <lucent/log.h>

#include <optional>
#include <vector>

namespace psx::cpu {
namespace {

void observeStore(const StoreObservation &observation, void * /*context*/) noexcept {
  lucent::debug("store-observe",
                "guest_pc=0x{:08X} phase={} cycle={} a0=0x{:08X} t0=0x{:08X} t1=0x{:08X} sp=0x{:08X} ra=0x{:08X}",
                observation.guestPc,
                observation.phase == StoreObservationPhase::Before ? "before" : "after",
                observation.guestCycle,
                observation.gpr[kA0],
                observation.gpr[kT0],
                observation.gpr[kT1],
                observation.gpr[kSp],
                observation.gpr[kRa]);
}

void disarm(Core &core) {
  core.lightrecExecutor().configureStoreObserver({}, nullptr, nullptr);
}

} // namespace

void configureStoreObserver(Core &core) {
  const std::optional<std::vector<std::uint32_t>> &pcs = psx::config::store_observe_pcs();
  if (!pcs) {
    lucent::error("store-observe", "refusing PSXPORT_STORE_OBSERVE: not a list of hex store PCs");
    disarm(core);
    return;
  }
  if (pcs->empty()) {
    disarm(core);
    return;
  }
  if (pcs->size() > kMaxObservedStoreTargets) {
    lucent::error("store-observe", "refusing PSXPORT_STORE_OBSERVE: more than {} store PCs", kMaxObservedStoreTargets);
    disarm(core);
    return;
  }
  const StoreObserverStatus status = core.lightrecExecutor().configureStoreObserver(*pcs, observeStore, nullptr);
  if (status != StoreObserverStatus::Configured) {
    lucent::error("store-observe", "could not arm the store observer (status {})", static_cast<int>(status));
    disarm(core);
    return;
  }
  lucent::info("store-observe", "watching {} store-instruction PC(s)", pcs->size());
  for (std::size_t i = 0; i < pcs->size(); ++i) {
    lucent::info("store-observe", "  [{}] store PC 0x{:08X}", i, (*pcs)[i]);
  }
}

void reportStoreObserver(const StoreObserverReport &report) {
  if (!report.armed && report.targetCount == 0) {
    return;
  }
  lucent::info("store-observe",
               "report: armed={} store_pcs={} jit_instructions={} fallback_instructions={} observations={}",
               report.armed ? "yes" : "no",
               report.targetCount,
               report.executedJitInstructions,
               report.fallbackInstructions,
               report.observations);
  for (std::size_t i = 0; i < report.targetCount && i < report.targets.size(); ++i) {
    const StoreObserverTargetCounts &target = report.targets[i];
    if (target.before + target.after == 0) {
      lucent::info("store-observe",
                   "  [{}] store PC 0x{:08X} never executed in {} JIT instruction(s)",
                   i,
                   target.guestPc,
                   report.executedJitInstructions);
      continue;
    }
    lucent::info("store-observe", "  [{}] store PC 0x{:08X} executed {} time(s)", i, target.guestPc, target.before);
    if (target.lastTarget.valid) {
      lucent::info("store-observe",
                   "      last write: address=0x{:08X} value=0x{:08X} (base $r{}, disp {:+d}, from $r{}){}",
                   target.lastTarget.address,
                   target.lastTarget.value,
                   target.lastTarget.baseRegister,
                   target.lastTarget.displacement,
                   target.lastTarget.sourceRegister,
                   target.distinctTargets == 0
                       ? ""
                       : lucent::format("; {} further address change(s)", target.distinctTargets));
      if (target.firstTarget.valid && target.firstTarget.address != target.lastTarget.address) {
        lucent::info("store-observe",
                     "      first write: address=0x{:08X} value=0x{:08X} (the store moved)",
                     target.firstTarget.address,
                     target.firstTarget.value);
      }
    }
  }
}

} // namespace psx::cpu
