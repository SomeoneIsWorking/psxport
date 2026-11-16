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
  return {
      lucent::format("guest: calls={} executed_blocks={} executed_instructions={} host_dispatches={} "
                     "memory_accesses={} device_clock_commits={} device_clock_commit_instructions={} invalidations={} "
                     "faults={}",
                     k.calls,
                     k.executedBlocks,
                     k.executedInstructions,
                     k.hostDispatches,
                     k.memoryAccesses,
                     k.deviceClockCommits,
                     k.deviceClockCommitInstructions,
                     k.invalidations,
                     k.faults),
      invalidationsBySource(k),
      lucent::format("budget_exit: exits={} pc_in_code_image={} pc_outside_code_image={}",
                     k.budgetExits,
                     k.budgetExitPcInCodeImage,
                     k.budgetExitPcOutsideCodeImage),
  };
}

void logLedger(const ExecutorCounters &counters, std::string_view phase, lucent::Level level) {
  for (const std::string &line : ledgerLines(counters)) {
    lucent::log(level, "guest", lucent::format("{}: {}", phase, line));
  }
}

} // namespace psx::cpu
