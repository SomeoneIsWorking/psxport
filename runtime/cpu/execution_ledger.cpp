#include "execution_ledger.h"

#include "executable_write_source.h"

#include <array>
#include <string_view>

namespace psx::cpu {
namespace {

// Indexed by ExecutableWriteSource; the static_assert keeps it the same length as the enum.
constexpr std::array<std::string_view, kExecutableWriteSourceCount> kWriteSourceNames{
    "cpu", "mapped_store", "dma", "module_load", "debugger", "savestate", "native"};

std::string invalidationsBySource(const ExecutorCounters &counters) {
  std::string line = "invalidations_by_source:";
  for (std::size_t source = 0; source < kExecutableWriteSourceCount; ++source) {
    line += lucent::format(" {}={}", kWriteSourceNames[source], counters.invalidationsBySource[source]);
  }
  return line;
}

} // namespace

std::vector<std::string> ledgerLines(const ExecutorCounters &k) {
  const InterpreterFallbackCounters &f = k.fallback;
  return {
      lucent::format("guest: calls={} translated_blocks={} executed_blocks={} executed_instructions={} "
                     "host_dispatches={} cache_hits={} cache_misses={} memory_callbacks={} device_clock_commits={} "
                     "device_clock_commit_instructions={} invalidations={} faults={}",
                     k.calls,
                     k.translatedBlocks,
                     k.executedBlocks,
                     k.executedInstructions,
                     k.hostDispatches,
                     k.cacheHits,
                     k.cacheMisses,
                     k.memoryCallbacks,
                     k.deviceClockCommits,
                     k.deviceClockCommitInstructions,
                     k.invalidations,
                     k.faults),
      invalidationsBySource(k),
      lucent::format("invalidation_work: lightrec_calls={} words_examined={} guarded_calls={} walks={} "
                     "block_scans={} revoked_blocks={}",
                     k.lightrecInvalidationCalls,
                     k.lightrecInvalidationWords,
                     k.lightrecInvalidationGuards,
                     k.lightrecInvalidationCalls - k.lightrecInvalidationGuards,
                     k.lightrecInvalidationBlockScans,
                     k.lightrecInvalidatedBlocks),
      lucent::format("budget_exit: exits={} pc_in_code_image={} pc_outside_code_image={}",
                     k.budgetExits,
                     k.budgetExitPcInCodeImage,
                     k.budgetExitPcOutsideCodeImage),
      lucent::format("fallback: max_blocks_per_execution={} calls={} instructions={} refused_calls={} "
                     "compilation_failed={} self_modifying_code={} unsupported_block={} load_delay_hazard={} "
                     "unsafe_instruction_fetch={} refused_compilation_failed={} refused_self_modifying_code={} "
                     "refused_unsupported_block={} refused_load_delay_hazard={} refused_unsafe_instruction_fetch={}",
                     k.maxFallbackBlocksPerExecution,
                     f.calls,
                     f.instructions,
                     f.refusedCalls,
                     f.compilationFailed,
                     f.selfModifyingCode,
                     f.unsupportedBlock,
                     f.loadDelayHazard,
                     f.unsafeInstructionFetch,
                     f.refusedCompilationFailed,
                     f.refusedSelfModifyingCode,
                     f.refusedUnsupportedBlock,
                     f.refusedLoadDelayHazard,
                     f.refusedUnsafeInstructionFetch),
  };
}

void logLedger(const ExecutorCounters &counters, std::string_view phase, lucent::Level level) {
  for (const std::string &line : ledgerLines(counters)) {
    lucent::log(level, "guest", lucent::format("{}: {}", phase, line));
  }
}

} // namespace psx::cpu
