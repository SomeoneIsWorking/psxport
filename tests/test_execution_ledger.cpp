// test_execution_ledger — the whole-run guest ledger names every counter and prints each one's own value.
//
// THE SHAPE THIS GUARDS: a ledger that omits a counter reads as "that counter is zero". The fallback
// reasons were the case — `load_delay_hazard` was a counter the executor kept and the report did not
// name. Every field below gets a DISTINCT value, so a report that transposes two fields (refused versus
// admitted, one reason's value under another's name) is as visible as one that drops a field.

#include "../runtime/cpu/execution_ledger.h"
#include "testutil.h"

#include <string>
#include <vector>

using psx::cpu::ExecutorCounters;

static ExecutorCounters distinctCounters() {
  ExecutorCounters k;
  k.calls = 101;
  k.translatedBlocks = 103;
  k.executedBlocks = 107;
  k.executedInstructions = 109;
  k.hostDispatches = 113;
  k.cacheHits = 127;
  k.cacheMisses = 131;
  k.memoryCallbacks = 137;
  k.invalidations = 139;
  k.faults = 149;
  k.budgetExits = 151;
  k.budgetExitPcInCodeImage = 157;
  k.budgetExitPcOutsideCodeImage = 163;
  k.lightrecInvalidationCalls = 307;
  k.lightrecInvalidationWords = 311;
  k.lightrecInvalidationGuards = 293;
  k.lightrecInvalidationBlockScans = 313;
  k.lightrecInvalidatedBlocks = 317;
  for (std::size_t source = 0; source < k.invalidationsBySource.size(); ++source) {
    k.invalidationsBySource[source] = 1000 + 11 * source;
  }
  k.fallback.calls = 211;
  k.fallback.instructions = 223;
  k.fallback.refusedCalls = 227;
  k.fallback.compilationFailed = 229;
  k.fallback.selfModifyingCode = 233;
  k.fallback.unsupportedBlock = 239;
  k.fallback.loadDelayHazard = 241;
  k.fallback.unsafeInstructionFetch = 251;
  k.fallback.refusedCompilationFailed = 257;
  k.fallback.refusedSelfModifyingCode = 263;
  k.fallback.refusedUnsupportedBlock = 269;
  k.fallback.refusedLoadDelayHazard = 271;
  k.fallback.refusedUnsafeInstructionFetch = 277;
  return k;
}

static std::string joined(const ExecutorCounters &k) {
  std::string text;
  for (const std::string &line : psx::cpu::ledgerLines(k)) {
    text += line + "\n";
  }
  return text;
}

// "key=value" followed by a space or end of line, so `calls=1` cannot match inside `refused_calls=1`.
static bool has(const std::string &text, const std::string &key, unsigned long long value) {
  const std::string needle = " " + key + "=" + std::to_string(value);
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    const size_t end = at + needle.size();
    if (end == text.size() || text[end] == ' ' || text[end] == '\n') {
      return true;
    }
  }
  return false;
}

static void test_every_counter_is_named_with_its_own_value(void) {
  const std::string text = joined(distinctCounters());
  const std::vector<std::pair<std::string, unsigned long long>> expected{
      {"calls", 101},
      {"translated_blocks", 103},
      {"executed_blocks", 107},
      {"executed_instructions", 109},
      {"host_dispatches", 113},
      {"cache_hits", 127},
      {"cache_misses", 131},
      {"memory_callbacks", 137},
      {"invalidations", 139},
      {"faults", 149},
      {"exits", 151},
      {"pc_in_code_image", 157},
      {"pc_outside_code_image", 163},
      {"lightrec_calls", 307},
      {"words_examined", 311},
      {"guarded_calls", 293},
      {"walks", 14}, // calls minus guarded calls, derived
      {"block_scans", 313},
      {"revoked_blocks", 317},
      {"cpu", 1000},
      {"mapped_store", 1011},
      {"dma", 1022},
      {"module_load", 1033},
      {"debugger", 1044},
      {"savestate", 1055},
      {"native", 1066},
      {"instructions", 223},
      {"refused_calls", 227},
      {"compilation_failed", 229},
      {"self_modifying_code", 233},
      {"unsupported_block", 239},
      {"load_delay_hazard", 241},
      {"unsafe_instruction_fetch", 251},
      {"refused_compilation_failed", 257},
      {"refused_self_modifying_code", 263},
      {"refused_unsupported_block", 269},
      {"refused_load_delay_hazard", 271},
      {"refused_unsafe_instruction_fetch", 277},
  };
  for (const auto &[key, value] : expected) {
    if (!has(text, key, value)) {
      PT_FAILED("ledger does not report %s=%llu:\n%s", key.c_str(), value, text.c_str());
    }
  }
  CHECK(has(text, "calls", 101));
  CHECK(has(text, "calls", 211)); // the fallback line's own `calls`, distinct from the executor's
}

static void test_a_zero_counter_is_printed_not_omitted(void) {
  const std::string text = joined(ExecutorCounters{});
  CHECK(has(text, "load_delay_hazard", 0));
  CHECK(has(text, "refused_load_delay_hazard", 0));
  CHECK(has(text, "translated_blocks", 0));
  CHECK(has(text, "native", 0));
}

static void test_the_checker_itself_rejects_a_transposed_value(void) {
  // Prove `has` can say no: a value under the wrong name must not match.
  const std::string text = joined(distinctCounters());
  CHECK(!has(text, "load_delay_hazard", 271)); // that is refused_load_delay_hazard's value
  CHECK(!has(text, "refused_load_delay_hazard", 241));
  CHECK(!has(text, "calls", 227)); // `refused_calls` must not satisfy `calls`
}

static void test_lines_are_five_named_groups(void) {
  const std::vector<std::string> lines = psx::cpu::ledgerLines(distinctCounters());
  CHECK_EQ(lines.size(), (size_t)5);
  CHECK(lines[0].rfind("guest:", 0) == 0);
  CHECK(lines[1].rfind("invalidations_by_source:", 0) == 0);
  CHECK(lines[2].rfind("invalidation_work:", 0) == 0);
  CHECK(lines[3].rfind("budget_exit:", 0) == 0);
  CHECK(lines[4].rfind("fallback:", 0) == 0);
}

int main(void) {
  RUN(every_counter_is_named_with_its_own_value);
  RUN(a_zero_counter_is_printed_not_omitted);
  RUN(the_checker_itself_rejects_a_transposed_value);
  RUN(lines_are_five_named_groups);
  return pt_summary();
}
