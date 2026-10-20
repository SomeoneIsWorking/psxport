// override_differential — shadows sampled calls of selected overrides: the original runs live and is
// journaled, the native replays that journal from the restored entry state, the two post-states are
// judged under the O32 contract, and the run continues from the original's state.
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

inline constexpr std::size_t kListedFurtherDifferences = 8;

struct DifferentialOutcome {
  DifferentialVerdict verdict = DifferentialVerdict::Match;
  // Mismatch: the first difference. Incomparable: `reason` names why, `difference` is empty.
  std::optional<DifferentialDifference> difference;
  // The differences after the first, up to kListedFurtherDifferences.
  std::vector<DifferentialDifference> further;
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
  // The largest sampled original journal, which sizes kMaxSideEffectsPerCall.
  std::size_t largestSampledCallEffects = 0;
  std::uint64_t largestSampledCallNumber = 0;
  // Sampled calls that stopped at the journal's per-call bound, and the first of them.
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
