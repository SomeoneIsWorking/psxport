#include "execution_ledger.h"

#include "executable_write_source.h"

#include <array>
#include <lucent/log.h>
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
                     "host_dispatches={} cache_hits={} cache_misses={} memory_callbacks={} invalidations={} faults={}",
                     k.calls,
                     k.translatedBlocks,
                     k.executedBlocks,
                     k.executedInstructions,
                     k.hostDispatches,
                     k.cacheHits,
                     k.cacheMisses,
                     k.memoryCallbacks,
                     k.invalidations,
                     k.faults),
      invalidationsBySource(k),
      // `budget_exits` is the denominator that makes the other two mean anything: with it "0 outside"
      // is a measurement; without it, it is indistinguishable from a run that never took a budget exit.
      // What the invalidations cost inside Lightrec: calls the guard answered alone versus walks over
      // every registered block, and how many blocks those walks examined and revoked.
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
      // Every reason the fallback counters carry: a report naming three of six reasons reads as "the
      // other three are zero" when it means "the other three were never asked".
      lucent::format("fallback: calls={} instructions={} refused_calls={} compilation_failed={} "
                     "self_modifying_code={} unsupported_block={} load_delay_hazard={} unsafe_instruction_fetch={} "
                     "refused_compilation_failed={} refused_self_modifying_code={} refused_unsupported_block={} "
                     "refused_load_delay_hazard={} refused_unsafe_instruction_fetch={}",
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

void logRunEndLedger(const ExecutorCounters &counters) {
  for (const std::string &line : ledgerLines(counters)) {
    lucent::info("guest", "run-end: {}", line);
  }
}

} // namespace psx::cpu
