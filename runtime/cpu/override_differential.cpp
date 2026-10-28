#include "override_differential.h"

#include "core.h"
#include "machine_snapshot.h"
#include "override_differential_contract.h"
#include "resumable_guest_call.h"
#include "side_effect_journal.h"

#include <lucent/log.h>

#include <algorithm>
#include <utility>

namespace psx::cpu {
namespace {

// A selected override reached inside either path runs normally rather than shadowing a half-run call.
class ShadowingScope {
public:
  explicit ShadowingScope(bool &flag) : flag_(flag) {
    flag_ = true;
  }
  ~ShadowingScope() {
    flag_ = false;
  }
  ShadowingScope(const ShadowingScope &) = delete;
  ShadowingScope &operator=(const ShadowingScope &) = delete;

private:
  bool &flag_;
};

DifferentialOutcome incomparable(std::string reason) {
  DifferentialOutcome outcome;
  outcome.verdict = DifferentialVerdict::Incomparable;
  outcome.reason = std::move(reason);
  return outcome;
}

// A journal overrun is a mismatch: comparing around a truncated log reports agreement never measured.
DifferentialOutcome
journalBoundFault(const DifferentialKeyStats &stats, const JournalBoundViolation &violation, std::size_t journaled) {
  DifferentialOutcome outcome;
  outcome.verdict = DifferentialVerdict::Mismatch;
  outcome.reason = lucent::format("{} @0x{:08X}: {}", stats.name, stats.key.address, violation.describe());
  outcome.sideEffectsOriginal = journaled;
  return outcome;
}

std::string outcomeDetail(const DifferentialOutcome &outcome) {
  if (outcome.verdict == DifferentialVerdict::Incomparable) {
    return outcome.reason;
  }
  if (!outcome.difference) {
    return lucent::format("{} side effect(s) replayed, {} dead-stack byte(s) ignored",
                          outcome.sideEffectsOriginal,
                          outcome.deadStackBytesIgnored);
  }
  std::string further;
  for (const DifferentialDifference &difference : outcome.further) {
    further += lucent::format("; {}: original {} native {}", difference.what, difference.original, difference.native);
  }
  return lucent::format("first difference {}: original {} native {} ({} register(s), {} memory range(s) / {} "
                        "byte(s) differ){}",
                        outcome.difference->what,
                        outcome.difference->original,
                        outcome.difference->native,
                        outcome.registersDiffering,
                        outcome.memoryRangesDiffering,
                        outcome.memoryBytesDiffering,
                        further);
}

} // namespace

const char *differentialVerdictName(DifferentialVerdict verdict) {
  switch (verdict) {
  case DifferentialVerdict::Match:
    return "match";
  case DifferentialVerdict::Mismatch:
    return "mismatch";
  case DifferentialVerdict::Incomparable:
    return "incomparable";
  }
  return "unknown";
}

OverrideDifferential::OverrideDifferential(Core &core, OverrideDifferentialConfig config)
    : core_(core), config_(std::move(config)) {
  std::string requested;
  for (const DifferentialSelector &selector : config_.selectors) {
    requested += requested.empty() ? selector.text : ", " + selector.text;
  }
  lucent::info("override-diff",
               "ARMED for {} selector(s) [{}]: sampling the first {} call(s) of each override and every {}th "
               "after; report {}",
               config_.selectors.size(),
               requested,
               config_.firstCalls,
               config_.everyKth,
               config_.reportPath);
  // Written at once so a run that never reaches a selected override still reports zero samples.
  (void)writeReport(false);
}

OverrideDifferential::~OverrideDifferential() {
  logSummary();
  (void)writeReport(true);
}

const OverrideDifferentialConfig &OverrideDifferential::config() const {
  return config_;
}

const std::vector<DifferentialKeyStats> &OverrideDifferential::keys() const {
  return keys_;
}

std::uint64_t OverrideDifferential::sampledFor(const DifferentialSelector &selector) const {
  std::uint64_t sampled = 0;
  for (const DifferentialKeyStats &stats : keys_) {
    if (selector.matches(stats.key.address, stats.name)) {
      sampled += stats.sampled;
    }
  }
  return sampled;
}

bool OverrideDifferential::selects(NativeKey key, std::string_view name) const {
  return std::any_of(config_.selectors.begin(), config_.selectors.end(), [&](const DifferentialSelector &selector) {
    return selector.matches(key.address, name);
  });
}

DifferentialKeyStats &OverrideDifferential::statsFor(NativeKey key, std::string_view name) {
  const auto found = std::find_if(keys_.begin(), keys_.end(), [&](const DifferentialKeyStats &stats) {
    return stats.key == key;
  });
  if (found != keys_.end()) {
    return *found;
  }
  DifferentialKeyStats stats;
  stats.key = key;
  stats.name = std::string(name);
  keys_.push_back(std::move(stats));
  return keys_.back();
}

bool OverrideDifferential::samples(const DifferentialKeyStats &stats) const {
  return stats.callsSeen <= config_.firstCalls || (config_.everyKth != 0 && stats.callsSeen % config_.everyKth == 0);
}

std::optional<ExecutionResult>
OverrideDifferential::intercept(NativeKey key, std::string_view name, NativeFunction function) {
  if (shadowing_ || !selects(key, name)) {
    return std::nullopt;
  }
  DifferentialKeyStats &stats = statsFor(key, name);
  ++stats.callsSeen;
  if (!samples(stats)) {
    return std::nullopt;
  }
  ++stats.sampled;
  return shadow(stats, function);
}

ExecutionResult OverrideDifferential::shadow(DifferentialKeyStats &stats, NativeFunction function) {
  const ShadowingScope shadowing(shadowing_);
  const NativeKey key = stats.key;
  const std::uint32_t entrySp = core_.r[kSp];
  const MachineSnapshot entry = MachineSnapshot::capture(core_);

  // 1. The original, live: its device traffic reaches the devices and is journaled.
  SideEffectJournal live(SideEffectMode::Record, {});
  ExecutionResult original;
  {
    const SideEffectJournal::Scope journal(core_, live);
    original = callOriginalResumingToExit(core_, stats.name, key);
  }
  if (!original.returned()) {
    // The run continues exactly where the original stopped; the native is never run for this call.
    record(stats,
           incomparable(lucent::format(
               "original did not return: {} at 0x{:08X}", executionExitName(original.reason), original.guestPc)));
    return original;
  }
  // Reported as the native it stands in for: its guest time was already charged by the nested run.
  original.cycles = 0;
  if (live.unreplayable()) {
    record(stats, incomparable(*live.unreplayable()));
    return original;
  }
  // A truncated original journal leaves nothing to replay; the native does not run.
  if (const auto &violation = live.boundViolation()) {
    record(stats, journalBoundFault(stats, *violation, live.effects().size()));
    return original;
  }
  if (live.effects().size() > stats.largestSampledCallEffects) {
    stats.largestSampledCallEffects = live.effects().size();
    stats.largestSampledCallNumber = stats.callsSeen;
  }
  const MachineSnapshot afterOriginal = MachineSnapshot::capture(core_);

  // 2. The native, against the restored entry state, with the original's journal replayed to it.
  stats.restoredRanges += entry.restoreInto(core_).rangesWritten;
  SideEffectJournal replay(SideEffectMode::Replay, live.effects());
  ExecutionResult native;
  bool stoppedAtBound = false;
  {
    const SideEffectJournal::Scope journal(core_, replay);
    // A runaway native is stopped by the journal bound; host frames only, so throwing here is safe.
    try {
      native = invokeNativeFunction(core_, key.address, function);
    } catch (const JournalBoundExceeded &) {
      stoppedAtBound = true;
    }
  }

  if (stoppedAtBound) {
    // 3. Continue from the original's state; the partial native state is discarded, not compared.
    stats.restoredRanges += afterOriginal.restoreInto(core_).rangesWritten;
    ++stats.journalBoundFaults;
    if (!stats.firstJournalBoundFault) {
      stats.firstJournalBoundFault = replay.boundViolation();
    }
    record(
        stats,
        journalBoundFault(stats, replay.boundViolation().value_or(JournalBoundViolation{}), replay.effects().size()));
    return original;
  }
  const MachineSnapshot afterNative = MachineSnapshot::capture(core_);

  // 3. Continue from the original's state, whatever the native did.
  stats.restoredRanges += afterOriginal.restoreInto(core_).rangesWritten;

  if (replay.unreplayable()) {
    record(stats, incomparable(*replay.unreplayable()));
  } else if (!native.returned()) {
    DifferentialOutcome outcome;
    outcome.verdict = DifferentialVerdict::Mismatch;
    outcome.difference = DifferentialDifference{
        "exit", "guest-return", lucent::format("{} at 0x{:08X}", executionExitName(native.reason), native.guestPc)};
    record(stats, std::move(outcome));
  } else {
    record(stats,
           judgeCompletedCall({afterOriginal, original, live.effects()},
                              {afterNative, native, replay.effects()},
                              DeadStack{entrySp, config_.deadStackBytes}));
  }
  return original;
}

void OverrideDifferential::record(DifferentialKeyStats &stats, DifferentialOutcome outcome) {
  switch (outcome.verdict) {
  case DifferentialVerdict::Match:
    ++stats.match;
    break;
  case DifferentialVerdict::Mismatch:
    ++stats.mismatch;
    if (!stats.firstMismatch) {
      stats.firstMismatch = outcome;
      stats.firstMismatchCall = stats.callsSeen;
    }
    break;
  case DifferentialVerdict::Incomparable:
    ++stats.incomparable;
    ++stats.incomparableByReason[outcome.reason];
    break;
  }
  stats.deadStackBytesIgnored += outcome.deadStackBytesIgnored;
  lucent::debug("override-diff-calls",
                "{} @0x{:08X} call {}: {} — {}",
                stats.name,
                stats.key.address,
                stats.callsSeen,
                differentialVerdictName(outcome.verdict),
                outcomeDetail(outcome));
  if (stats.lastVerdict == outcome.verdict) {
    return;
  }
  lucent::info("override-diff",
               "{} @0x{:08X} call {} (sample {}): verdict {} -> {} — {}",
               stats.name,
               stats.key.address,
               stats.callsSeen,
               stats.sampled,
               stats.lastVerdict ? differentialVerdictName(*stats.lastVerdict) : "none",
               differentialVerdictName(outcome.verdict),
               outcomeDetail(outcome));
  stats.lastVerdict = outcome.verdict;
  (void)writeReport(false);
}

} // namespace psx::cpu
