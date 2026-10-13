#include "run_end_report.h"

#include "config_vars.h"
#include "core.h"
#include "execution_ledger.h"
#include "game.h"
#include "game_runtime.h"
#include "lightrec_executor.h"

#include <lucent/log.h>

#include <cstdio>
#include <string>

namespace psx {

void RunEndReport::emit(std::uint64_t productSteps) const {
  Core &core = game_.core;
  const auto &counts = core.lightrecExecutor().counters();
  lucent::info("runtime",
               "run complete: product_steps={} presentation_fences={} "
               "translated_blocks={} executed_blocks={} executed_instructions={} "
               "cache_hits={} cache_misses={} host_dispatches={} invalidations={} faults={}",
               productSteps,
               game_.presentation.fence(),
               counts.translatedBlocks,
               counts.executedBlocks,
               counts.executedInstructions,
               counts.cacheHits,
               counts.cacheMisses,
               counts.hostDispatches,
               counts.invalidations,
               counts.faults);
  core.lightrecExecutor().reportFallbackTelemetry("run-complete");
  psx::cpu::logRunEndLedger(counts);
  core.guestCallCensus().log("product loop ended");
  game_.perf.reportPresents();
  game_.presentation.reportStability();
  game_.spu_audio.reportQueue();
  dumpRamIfRequested();
  if (core.runtime != nullptr) {
    core.runtime->reportRun(core);
  }
}

// PSXPORT_RAMDUMP: the after-loop dump. A run that ends by exiting its process never gets here;
// PSXPORT_RAMDUMP_FRAME (FieldTurn) is the mid-run one.
void RunEndReport::dumpRamIfRequested() const {
  const std::string &path = psx::config::cv_ramdump.get();
  if (path.empty()) {
    return;
  }
  FILE *dump = fopen(path.c_str(), "wb");
  if (dump == nullptr) {
    lucent::error("run", "end-of-run RAM dump could not open {}", path);
    return;
  }
  fwrite(game_.core.ram, 1, 0x200000, dump);
  fclose(dump);
  lucent::info("run", "dumped 2MB RAM -> {}", path);
}

} // namespace psx
