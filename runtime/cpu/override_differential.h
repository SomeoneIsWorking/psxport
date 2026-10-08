// override_differential — the per-function gate every new native override must pass before landing.
//
// WHAT IT DOES. For each configured override (by registered name or guest entry address) it SHADOWS a
// sample of real calls inside a real run:
//
//   1. snapshot the CPU (GPRs, HI/LO, PC, COP0), GTE registers, main RAM and scratchpad;
//   2. run the ORIGINAL guest body to its return (native suppressed for that key, resumed across budget
//      exhaustion), journaling every device access and host service it makes — live;
//   3. capture that post-state A, then restore the snapshot, invalidating Lightrec for every range whose
//      bytes change so no block translated from the replaced bytes survives;
//   4. run the NATIVE override against the restored state with its device traffic REPLAYED from step 2's
//      journal (reads served from the log, writes compared and swallowed, guest time held);
//   5. capture post-state B, diff A against B under the MIPS O32 contract, restore A, and continue the run
//      from A — the original's result — so the game stays faithful whatever the override did.
//
// THE CONTRACT COMPARED. v0, v1, s0-s7, gp, sp, s8/fp and ra must match, and so must the continuation PC,
// COP0 Status and all 64 GTE registers. Caller-saved temporaries (at, a0-a3, t0-t9, k0/k1, HI/LO) are not
// compared. Every main-RAM and scratchpad byte must match, except differences inside the callee-frame
// window below the entry `sp` (`OverrideDifferentialConfig::deadStackBytes`), which are counted and
// reported but not judged. The ordered device/host-service journals must be equal.
//
// WHAT IT CANNOT COMPARE, and what the verdict then says. A call whose original performs anything the
// journal cannot replay — a BIOS or platform-HLE service, a syscall, pending interrupt/host-turn work — or
// exits for any reason other than returning, is INCOMPARABLE with that reason, never a match; the native
// is not run for it. A native that performs such a service is likewise incomparable. Device models reached
// by native code DIRECTLY in C++, bypassing `Core`'s memory API, are not observed by any of this.
//
// THE JOURNAL'S PER-CALL BOUND IS A MISMATCH, not an incomparable. One call producing more than
// `psx::cpu::kMaxSideEffectsPerCall` effects is a runaway — measured, where a
// candidate override looped inside one call and grew the journal to 7.4 GB — and a verdict that lets
// such a call pass as "nothing to compare" is the failure the bound exists to prevent. Either path
// reaching the bound is a MISMATCH naming the override, its address, the path, the effect, and the
// bound; the run continues from the ORIGINAL's state either way, and a native stopped at the bound is
// stopped by an unwind out of its own body, never by a `throw` across a translated Lightrec frame.
//
// Output: one log line per verdict CHANGE per override, a summary with denominators at shutdown, and a
// machine-readable JSON report (`tools/override_differential_gate.py` gates on it). A requested selector
// that sampled zero calls is a FAILURE in both.
#pragma once

#include "execution_exit.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "override_differential_config.h"
#include "side_effect_journal.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Core;

namespace psx::cpu {

enum class DifferentialVerdict : std::uint8_t {
  Match,
  Mismatch,
  Incomparable,
};

const char *differentialVerdictName(DifferentialVerdict verdict);

// The first thing that differed in one call, with both paths' values rendered as text.
struct DifferentialDifference {
  std::string what; // e.g. "register v0", "ram [0x00080000,0x00080004)", "side effect #2"
  std::string original;
  std::string native;
};

struct DifferentialOutcome {
  DifferentialVerdict verdict = DifferentialVerdict::Match;
  // Mismatch: the first difference. Incomparable: `reason` names why, `difference` is empty.
  std::optional<DifferentialDifference> difference;
  std::string reason;
  std::uint32_t registersDiffering = 0;
  std::uint32_t memoryRangesDiffering = 0;
  std::uint64_t memoryBytesDiffering = 0;
  std::uint64_t deadStackBytesIgnored = 0;
  std::uint64_t sideEffectsOriginal = 0;
  std::uint64_t sideEffectsNative = 0;
};

struct DifferentialKeyStats {
  NativeKey key{};
  std::string name;
  std::uint64_t callsSeen = 0;
  std::uint64_t sampled = 0;
  std::uint64_t match = 0;
  std::uint64_t mismatch = 0;
  std::uint64_t incomparable = 0;
  std::map<std::string, std::uint64_t> incomparableByReason;
  std::uint64_t deadStackBytesIgnored = 0;
  std::uint64_t restoredRanges = 0;
  std::optional<std::uint64_t> firstMismatchCall;
  std::optional<DifferentialOutcome> firstMismatch;
  std::optional<DifferentialVerdict> lastVerdict;
  // THE LARGEST SAMPLED CALL'S JOURNAL, and the total the bound above it. These are the measurement
  // `psx::cpu::kMaxSideEffectsPerCall` is sized from, reported on every run so the constant's comment
  // is checkable against a real corpus instead of being a number nobody can test. Both are counts
  // across the key's sampled calls, so a key that was never sampled reports 0 of 0 — the same zero
  // that means "scanned and found none" only when `sampled` is non-zero, which the summary states.
  std::size_t largestSampledCallEffects = 0;
  std::uint64_t largestSampledCallNumber = 0;
  // Sampled calls that stopped at the journal's per-call bound, and the first of them. A non-zero count
  // is a gate failure; it is recorded separately from `mismatch` so a reader can tell "the two paths
  // differ" from "one path could not be journaled at all".
  std::uint64_t journalBoundFaults = 0;
  std::optional<psx::cpu::JournalBoundViolation> firstJournalBoundFault;
};

class OverrideDifferential {
public:
  OverrideDifferential(Core &core, OverrideDifferentialConfig config);
  // Logs the summary and writes the final report.
  ~OverrideDifferential();
  OverrideDifferential(const OverrideDifferential &) = delete;
  OverrideDifferential &operator=(const OverrideDifferential &) = delete;

  // The dispatcher's hook for one call of an installed override. Returns the call's result when this
  // call was shadowed (the run continues from the ORIGINAL's state), or nullopt when the key is not
  // selected or this call is not sampled — the dispatcher then invokes the native as usual.
  std::optional<ExecutionResult> intercept(NativeKey key, std::string_view name, NativeFunction function);

  const OverrideDifferentialConfig &config() const;
  const std::vector<DifferentialKeyStats> &keys() const;
  // Sampled calls, summed over every key a selector matched.
  std::uint64_t sampledFor(const DifferentialSelector &selector) const;
  // The JSON report; `complete` is false for a mid-run write.
  std::string reportJson(bool complete) const;
  bool writeReport(bool complete) const;

private:
  bool selects(NativeKey key, std::string_view name) const;
  DifferentialKeyStats &statsFor(NativeKey key, std::string_view name);
  bool samples(const DifferentialKeyStats &stats) const;
  ExecutionResult shadow(DifferentialKeyStats &stats, NativeFunction function);
  void record(DifferentialKeyStats &stats, DifferentialOutcome outcome);
  void logSummary() const;

  Core &core_;
  OverrideDifferentialConfig config_;
  std::vector<DifferentialKeyStats> keys_;
  bool shadowing_ = false;
};

} // namespace psx::cpu
