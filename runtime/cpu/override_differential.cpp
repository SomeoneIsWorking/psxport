#include "override_differential.h"

#include "core.h"
#include "machine_snapshot.h"
#include "override_differential_contract.h"
#include "side_effect_journal.h"

#include <lucent/log.h>

#include <algorithm>
#include <utility>

namespace psx::cpu {
namespace {

inline constexpr std::uint32_t kStackPointer = 29;

// Holds `flag` for one shadowed call so a selected override reached INSIDE either path is dispatched
// normally rather than shadowed again: nesting would snapshot a half-run call and double-count it.
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

// THE JOURNAL'S PER-CALL BOUND, AS A VERDICT.
//
// It is a MISMATCH and not an INCOMPARABLE because those two answers mean opposite things to whoever
// reads the gate. Incomparable is a statement about the CALL: "there was nothing here to compare", and
// `failuresOf` treats a selector whose samples are all incomparable as a failure anyway, but a single
// incomparable call among comparable ones is tolerated. A bound violation is a statement about a
// LOG that could not be completed, so comparing around it would report agreement the run never
// measured. Reporting it as mismatch makes the run FAIL, which is what the bound exists for: a
// runaway override must come back as a verdict, not as a skip.
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
  // Written at once, so a run that never reaches a selected override still leaves a report — one that
  // says zero calls were sampled, which the gate reads as the failure it is.
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
    const bool matches = selector.address ? stats.key.address == *selector.address : stats.name == selector.text;
    if (matches) {
      sampled += stats.sampled;
    }
  }
  return sampled;
}

bool OverrideDifferential::selects(NativeKey key, std::string_view name) const {
  return std::any_of(config_.selectors.begin(), config_.selectors.end(), [&](const DifferentialSelector &selector) {
    return selector.address ? key.address == *selector.address : name == selector.text;
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
  const std::uint32_t entrySp = core_.r[kStackPointer];
  const MachineSnapshot entry = MachineSnapshot::capture(core_);

  // 1. The original, live: its device traffic reaches the devices and is journaled.
  SideEffectJournal live(SideEffectMode::Record, {});
  ExecutionResult original;
  {
    const SideEffectJournal::Scope journal(core_, live);
    original = callOriginalResumingToExit(core_, key, ExecutionBudget::currentTurn(core_));
  }
  if (!original.returned()) {
    // The run continues exactly where the original stopped; the native is never run for this call.
    record(stats,
           incomparable(lucent::format(
               "original did not return: {} at 0x{:08X}", executionExitName(original.reason), original.guestPc)));
    return original;
  }
  // Reported exactly as `invokeNativeFunction` reports the native it stands in for: a returned host
  // service with no cycles of its own. The original's guest time was already accounted by the nested
  // execution, so adding its cycles here would charge the caller's budget for them a second time.
  original.cycles = 0;
  original.detail = stats.name;
  if (live.unreplayable()) {
    record(stats, incomparable(*live.unreplayable()));
    return original;
  }
  // THE ORIGINAL OVERRAN THE JOURNAL'S PER-CALL BOUND. Its log is truncated, so there is nothing to
  // replay and the native must not run: the verdict is the fault, and the run continues from the
  // original's state exactly as every other outcome does.
  if (const auto &violation = live.boundViolation()) {
    record(stats, journalBoundFault(stats, *violation, live.effects().size()));
    return original;
  }
  // The measurement `kMaxSideEffectsPerCall` is sized from, taken on the ORIGINAL's journal because
  // that is the log the shadow path must reproduce — a native that made more effects than the original
  // is already a mismatch the judge reports, and its own count is not what sizes the storage.
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
    // THE RUNAWAY CASE. A native override that never returns kept appending to
    // `replay` at ~6 MB/s until the host died. The journal now refuses past its per-call bound and
    // raises out of HERE, where every frame is host code: a native body is the one place on a shadow
    // path with no translated frame below it, which is why `refuseAtBound` throws only in that case.
    //
    // Nothing is decided in the catch. `NativeExecutionScope` has already restored the core's PC and
    // active address by the time it runs, and the journal stays ATTACHED until this scope ends — so the
    // state restore, the snapshot and the verdict all happen below with the journal detached, exactly
    // as they do on every other path. Deciding here would mean writing to the Core and to `stats` from
    // inside a catch, which is how a fault path ends up behaving differently from a normal one.
    try {
      native = invokeNativeFunction(core_, key.address, function, stats.name);
    } catch (const JournalBoundExceeded &) {
      stoppedAtBound = true;
    }
  }

  if (stoppedAtBound) {
    // 3. Continue from the original's state. The native was stopped MID-LOOP, so its state is DISCARDED
    // rather than compared — comparing a partial body to a complete one would judge a call that never
    // finished. Only the original's result reaches the caller.
    stats.restoredRanges += afterOriginal.restoreInto(core_).rangesWritten;
    ++stats.journalBoundFaults;
    if (!stats.firstJournalBoundFault) {
      stats.firstJournalBoundFault = replay.boundViolation();
    }
    // `refuseAtBound` records the violation before it raises, so it is present by construction. The
    // `value_or` is a total function over an absent record rather than an unhandled case, and its
    // `describe()` still names the bound — so even a lost record reports the bound rather than nothing.
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
                              entrySp,
                              config_.deadStackBytes));
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
