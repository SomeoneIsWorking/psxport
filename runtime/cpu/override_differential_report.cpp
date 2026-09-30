// The override differential's two outputs: the shutdown summary through the logger and the
// machine-readable JSON report `tools/override_differential_gate.py` gates on. The failure rules are
// stated here and re-derived by the gate from the raw counts, so neither trusts the other's verdict.
#include "override_differential.h"

#include "fs_util.h"

#include <lucent/log.h>

#include <string>
#include <vector>

namespace psx::cpu {
namespace {

inline constexpr std::string_view kSchema = "psxport.override-differential/1";
inline constexpr std::string_view kCompared =
    "MIPS O32 results and callee-saved registers (v0 v1 s0-s7 gp sp s8 ra), the continuation pc, COP0 "
    "status, all 64 GTE registers, every main-RAM and scratchpad byte outside the dead-stack window below "
    "the entry sp, and the ordered log of device accesses and host services made through Core's memory API "
    "and guest dispatch";
inline constexpr std::string_view kNotObserved =
    "device or host state a native override changes by calling a device model directly in C++ rather than "
    "through Core's memory API; caller-saved registers (at a0-a3 t0-t9 k0 k1 hi lo)";

std::string jsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += lucent::format("\\u{:04x}", static_cast<unsigned>(c));
      } else {
        out += c;
      }
    }
  }
  out += '"';
  return out;
}

std::string hexAddress(std::uint32_t address) {
  return jsonString(lucent::format("0x{:08X}", address));
}

bool selectorMatches(const DifferentialSelector &selector, const DifferentialKeyStats &stats) {
  return selector.address ? stats.key.address == *selector.address : stats.name == selector.text;
}

// The rules the gate re-derives: a requested selector with zero sampled calls, or with samples none of
// which could be compared, proves nothing and fails; any mismatch fails.
std::vector<std::string> failuresOf(const OverrideDifferential &differential) {
  std::vector<std::string> failures;
  for (const DifferentialSelector &selector : differential.config().selectors) {
    std::uint64_t keys = 0;
    std::uint64_t compared = 0;
    for (const DifferentialKeyStats &stats : differential.keys()) {
      if (selectorMatches(selector, stats)) {
        ++keys;
        compared += stats.match + stats.mismatch;
      }
    }
    const std::uint64_t sampled = differential.sampledFor(selector);
    if (sampled == 0) {
      failures.push_back(lucent::format(
          "selector '{}' sampled 0 calls ({} installed override(s) matched it and were called)", selector.text, keys));
    } else if (compared == 0) {
      failures.push_back(
          lucent::format("selector '{}' sampled {} call(s) and every one was incomparable", selector.text, sampled));
    }
  }
  for (const DifferentialKeyStats &stats : differential.keys()) {
    if (stats.mismatch != 0) {
      failures.push_back(lucent::format("{} @0x{:08X}: {} of {} sampled call(s) mismatched",
                                        stats.name,
                                        stats.key.address,
                                        stats.mismatch,
                                        stats.sampled));
    }
  }
  // A BOUND FAULT IS REPORTED SEPARATELY from a mismatch, and stated in full. A mismatch means the two
  // paths completed and disagreed; a bound fault means one of them could not be journaled at all, so a
  // reader shown only the mismatch count would be told the wrong kind of thing happened. The first one
  // is carried whole — path, offending effect, the bound, whether the call was stopped — because that
  // sentence is the whole diagnosis.
  for (const DifferentialKeyStats &stats : differential.keys()) {
    if (stats.journalBoundFaults == 0) {
      continue;
    }
    failures.push_back(lucent::format("{} @0x{:08X}: {} of {} sampled call(s) reached the {}-effect per-call journal "
                                      "bound; first at call {}: {}",
                                      stats.name,
                                      stats.key.address,
                                      stats.journalBoundFaults,
                                      stats.sampled,
                                      psx::cpu::kMaxSideEffectsPerCall,
                                      stats.firstMismatchCall.value_or(0),
                                      stats.firstJournalBoundFault ? stats.firstJournalBoundFault->describe()
                                                                   : std::string("violation not retained")));
  }
  return failures;
}

std::string mismatchJson(const DifferentialKeyStats &stats) {
  if (!stats.firstMismatch || !stats.firstMismatch->difference) {
    return "null";
  }
  const DifferentialOutcome &outcome = *stats.firstMismatch;
  return lucent::format("{{\"call\": {}, \"what\": {}, \"original\": {}, \"native\": {}, \"registers_differing\": {}, "
                        "\"memory_ranges_differing\": {}, \"memory_bytes_differing\": {}}}",
                        stats.firstMismatchCall.value_or(0),
                        jsonString(outcome.difference->what),
                        jsonString(outcome.difference->original),
                        jsonString(outcome.difference->native),
                        outcome.registersDiffering,
                        outcome.memoryRangesDiffering,
                        outcome.memoryBytesDiffering);
}

// The journal's per-call bound, as data. `null` when no sampled call reached it, which is the ordinary
// case and must read as "no bound fault" rather than as an absent measurement: `journal_bound_faults`
// beside it is the denominator, so 0 faults with a non-zero `sampled` means "scanned, none reached the
// bound", and 0 of 0 means the key was never sampled.
std::string journalBoundJson(const DifferentialKeyStats &stats) {
  if (!stats.firstJournalBoundFault) {
    return "null";
  }
  const psx::cpu::JournalBoundViolation &violation = *stats.firstJournalBoundFault;
  return lucent::format(
      "{{\"call\": {}, \"path\": {}, \"bound\": {}, \"effects_journaled\": {}, \"effects_refused\": {}, "
      "\"stopped_call\": {}, \"offending\": {}}}",
      stats.firstMismatchCall.value_or(0),
      jsonString(violation.mode == psx::cpu::SideEffectMode::Record ? "original" : "native"),
      psx::cpu::kMaxSideEffectsPerCall,
      violation.effects,
      violation.refused,
      violation.stoppedCall ? "true" : "false",
      jsonString(psx::cpu::describeSideEffect(violation.offending)));
}

std::string keyJson(const DifferentialKeyStats &stats) {
  std::string reasons;
  for (const auto &[reason, count] : stats.incomparableByReason) {
    reasons += lucent::format("{}{}: {}", reasons.empty() ? "" : ", ", jsonString(reason), count);
  }
  return lucent::format(
      "{{\"name\": {}, \"address\": {}, \"image_id\": {}, \"image_generation\": {}, "
      "\"calls_seen\": {}, \"sampled\": {}, \"match\": {}, \"mismatch\": {}, \"incomparable\": {}, "
      "\"incomparable_by_reason\": {{{}}}, \"dead_stack_bytes_ignored\": {}, \"restored_ranges\": {}, "
      "\"largest_sampled_call_effects\": {}, \"largest_sampled_call_number\": {}, "
      "\"effects_bound\": {}, \"journal_bound_faults\": {}, \"first_journal_bound\": {}, "
      "\"first_mismatch\": {}}}",
      jsonString(stats.name),
      hexAddress(stats.key.address),
      stats.key.image.id,
      stats.key.image.generation,
      stats.callsSeen,
      stats.sampled,
      stats.match,
      stats.mismatch,
      stats.incomparable,
      reasons,
      stats.deadStackBytesIgnored,
      stats.restoredRanges,
      stats.largestSampledCallEffects,
      stats.largestSampledCallNumber,
      psx::cpu::kMaxSideEffectsPerCall,
      stats.journalBoundFaults,
      journalBoundJson(stats),
      mismatchJson(stats));
}

template <typename Items, typename Render> std::string jsonArray(const Items &items, Render render) {
  std::string out = "[";
  for (const auto &item : items) {
    out += lucent::format("{}\n    {}", out.size() > 1 ? "," : "", render(item));
  }
  out += out.size() > 1 ? "\n  ]" : "]";
  return out;
}

} // namespace

std::string OverrideDifferential::reportJson(bool complete) const {
  const std::string selectors = jsonArray(config_.selectors, [&](const DifferentialSelector &selector) {
    std::uint64_t keys = 0;
    for (const DifferentialKeyStats &stats : keys_) {
      keys += selectorMatches(selector, stats) ? 1u : 0u;
    }
    return lucent::format("{{\"selector\": {}, \"address\": {}, \"keys_matched\": {}, \"sampled\": {}}}",
                          jsonString(selector.text),
                          selector.address ? hexAddress(*selector.address) : std::string("null"),
                          keys,
                          sampledFor(selector));
  });
  const std::string keys = jsonArray(keys_, keyJson);
  const std::string failures = jsonArray(failuresOf(*this), [](const std::string &failure) {
    return jsonString(failure);
  });
  return lucent::format("{{\n  \"schema\": {},\n  \"complete\": {},\n  \"compared\": {},\n  \"not_observed\": {},\n"
                        "  \"config\": {{\"first_calls\": {}, \"every_kth\": {}, \"dead_stack_bytes\": {}}},\n"
                        "  \"selectors\": {},\n  \"keys\": {},\n  \"failures\": {}\n}}\n",
                        jsonString(kSchema),
                        complete ? "true" : "false",
                        jsonString(kCompared),
                        jsonString(kNotObserved),
                        config_.firstCalls,
                        config_.everyKth,
                        config_.deadStackBytes,
                        selectors,
                        keys,
                        failures);
}

bool OverrideDifferential::writeReport(bool complete) const {
  const std::string json = reportJson(complete);
  if (!Fs::writeFile(config_.reportPath, json.data(), json.size())) {
    lucent::error("override-diff", "could not write the report to {}", config_.reportPath);
    return false;
  }
  return true;
}

void OverrideDifferential::logSummary() const {
  for (const DifferentialKeyStats &stats : keys_) {
    std::string reasons;
    for (const auto &[reason, count] : stats.incomparableByReason) {
      reasons += lucent::format("{}{} x{}", reasons.empty() ? "" : "; ", reason, count);
    }
    lucent::info("override-diff",
                 "summary {} @0x{:08X}: {} call(s) seen, {} sampled: {} match, {} mismatch, {} incomparable{}{}",
                 stats.name,
                 stats.key.address,
                 stats.callsSeen,
                 stats.sampled,
                 stats.match,
                 stats.mismatch,
                 stats.incomparable,
                 reasons.empty() ? "" : " — ",
                 reasons);
    // THE MEASUREMENT THE JOURNAL'S BOUND IS SIZED FROM, with its denominator on the same line, and
    // the two cases named apart. A key whose sampled calls journaled no effect at all (a pure-RAM
    // function) and a key that was NEVER SAMPLED both report a largest count of 0, and those are
    // different facts: the first is a measurement of zero, the second is no measurement.
    lucent::info("override-diff",
                 "summary {} @0x{:08X}: largest sampled call journaled {} effect(s) (call {} of {} "
                 "sampled) against the {}-effect per-call bound; {} call(s) reached the bound{}",
                 stats.name,
                 stats.key.address,
                 stats.largestSampledCallEffects,
                 stats.largestSampledCallNumber,
                 stats.sampled,
                 psx::cpu::kMaxSideEffectsPerCall,
                 stats.journalBoundFaults,
                 stats.sampled == 0 ? " — NOT MEASURED, 0 call(s) were sampled" : "");
  }
  const std::vector<std::string> failures = failuresOf(*this);
  for (const std::string &failure : failures) {
    lucent::error("override-diff", "FAILED: {}", failure);
  }
  lucent::info("override-diff",
               "summary: {} selector(s), {} override key(s) called, {} failure(s); report {}",
               config_.selectors.size(),
               keys_.size(),
               failures.size(),
               config_.reportPath);
}

} // namespace psx::cpu
