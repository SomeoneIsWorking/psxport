#include "store_observe.h"

#include "config_vars.h" // cv_store_observe — the addresses to watch
#include "core.h"
#include "lightrec_executor.h"

#include <lucent/log.h>

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

using psx::cpu::kMaxObservedStoreTargets;
using psx::cpu::StoreObservation;
using psx::cpu::StoreObservationPhase;
using psx::cpu::StoreObserverReport;
using psx::cpu::StoreObserverStatus;
using psx::cpu::StoreObserverTargetCounts;

namespace {

// Per-run observation state. The callback is a plain function pointer with a `void*` context, so what
// it was armed with has to travel alongside it; that is what this owns.
struct ArmedTargets {
  std::array<std::uint32_t, kMaxObservedStoreTargets> addresses{};
  std::size_t count = 0;
  std::uint64_t observations = 0;
};

ArmedTargets g_armed;

void observeStore(const StoreObservation &observation, void *context) noexcept {
  auto *armed = static_cast<ArmedTargets *>(context);
  if (armed == nullptr) {
    return;
  }
  ++armed->observations;
  // `StoreObservation` carries the guest PC, the phase, the segment-relative cycle and the full
  // register file — but NOT the address that was written, so this line cannot name which of several
  // watched words fired. It deliberately does not invent one: with more than one address armed, run
  // them one at a time, or read `store_observe_report`, whose per-target rows DO carry the guest PC
  // and the before/after counts.
  //
  // $a0/$t0/$t1 are logged because a store's data register is usually one of them, and the two
  // scratch registers are what a caller compares first when hunting who wrote a word.
  lucent::debug("store-observe",
                "guest_pc=0x{:08X} phase={} cycle={} a0=0x{:08X} t0=0x{:08X} t1=0x{:08X} "
                "gpr[29]=0x{:08X} gpr[31]=0x{:08X} seen={}",
                observation.guestPc,
                observation.phase == StoreObservationPhase::Before ? "before" : "after",
                observation.guestCycle,
                observation.gpr.size() > 4 ? observation.gpr[4] : 0u,
                observation.gpr.size() > 8 ? observation.gpr[8] : 0u,
                observation.gpr.size() > 9 ? observation.gpr[9] : 0u,
                observation.gpr.size() > 29 ? observation.gpr[29] : 0u,
                observation.gpr.size() > 31 ? observation.gpr[31] : 0u,
                armed->observations);
}

// Parse the configured list. Returns false and names the offending text rather than watching a prefix.
bool parseTargets(std::string_view text, std::array<std::uint32_t, kMaxObservedStoreTargets> &out, std::size_t &count) {
  count = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ',' || text[at] == ' ' || text[at] == '\t')) {
      ++at;
    }
    if (at >= text.size()) {
      break;
    }
    std::size_t end = at;
    while (end < text.size() && text[end] != ',' && text[end] != ' ' && text[end] != '\t') {
      ++end;
    }
    const std::string token(text.substr(at, end - at));
    char *stop = nullptr;
    const unsigned long value = std::strtoul(token.c_str(), &stop, 16);
    if (stop == token.c_str() || *stop != '\0' || value == 0) {
      lucent::error("store-observe", "refusing PSXPORT_STORE_OBSERVE: '{}' is not a hex guest address", token.c_str());
      return false;
    }
    if (count == out.size()) {
      lucent::error("store-observe",
                    "refusing PSXPORT_STORE_OBSERVE: more than {} addresses (kMaxObservedStoreTargets), so "
                    "watching a prefix would report less than it was asked for",
                    out.size());
      return false;
    }
    out[count++] = static_cast<std::uint32_t>(value);
    at = end;
  }
  return true;
}

} // namespace

void store_observe_attach(Core &core) {
  store_observe_configure(core);
}

void store_observe_configure(Core &core) {
  const std::string_view requested = psx::config::cv_store_observe.get();
  if (requested.empty()) {
    // The default. Disarming an already-disarmed observer is a no-op, not an error, so a title may
    // call this unconditionally.
    core.lightrecExecutor().configureStoreObserver({}, nullptr, nullptr);
    return;
  }

  ArmedTargets armed;
  if (!parseTargets(requested, armed.addresses, armed.count) || armed.count == 0) {
    lucent::error("store-observe", "store observation is OFF; no addresses were watched");
    core.lightrecExecutor().configureStoreObserver({}, nullptr, nullptr);
    return;
  }

  const StoreObserverStatus status = core.lightrecExecutor().configureStoreObserver(
      std::span<const std::uint32_t>(armed.addresses.data(), armed.count), observeStore, &armed);
  if (status != StoreObserverStatus::Configured) {
    lucent::error("store-observe",
                  "could not arm the store observer (status {}); watching nothing. An UNARMED observer "
                  "reports nothing, so any conclusion drawn from its silence would be wrong",
                  static_cast<int>(status));
    core.lightrecExecutor().configureStoreObserver({}, nullptr, nullptr);
    return;
  }
  g_armed = armed;
  // The denominator, stated at arming: how many addresses, and which. A later "the observer saw
  // nothing" means something only against this line.
  lucent::info("store-observe", "watching {} guest address(es) for stores:", armed.count);
  for (std::size_t i = 0; i < armed.count; ++i) {
    lucent::info("store-observe", "  [{}] 0x{:08X}", i, armed.addresses[i]);
  }
}

void store_observe_report(Core &core) {
  const StoreObserverReport report = core.lightrecExecutor().storeObserverReport();
  if (!report.armed && report.targetCount == 0) {
    return; // never armed: the arming line already said so, and silence here is not evidence
  }
  // The executor's own counters, so "the observer ran and matched nothing" is distinguishable from
  // "the instrument never ran" — the distinction this area has got wrong before.
  lucent::info("store-observe",
               "report: armed={} targets={} jit_instructions={} fallback_instructions={} callback_lines={}",
               report.armed ? "yes" : "no",
               report.targetCount,
               report.executedJitInstructions,
               report.fallbackInstructions,
               g_armed.observations);
  for (std::size_t i = 0; i < report.targetCount && i < report.targets.size(); ++i) {
    const StoreObserverTargetCounts &target = report.targets[i];
    if (target.before + target.after == 0) {
      // Say "matched none" in words, and do NOT print a guest PC: with no observation there is no last
      // guest PC, and echoing the target address back in that column reads exactly like a hit. The
      // instrument's own scan size is what makes this line meaningful rather than merely empty.
      lucent::info("store-observe",
                   "  [{}] 0x{:08X} stores before=0 after=0 — MATCHED NONE of the {} executed JIT "
                   "instruction(s); this address was not written in this run",
                   i,
                   g_armed.addresses[i],
                   report.executedJitInstructions);
      continue;
    }
    lucent::info("store-observe",
                 "  [{}] 0x{:08X} stores before={} after={} last_guest_pc=0x{:08X}",
                 i,
                 g_armed.addresses[i],
                 target.before,
                 target.after,
                 target.guestPc);
  }
}
