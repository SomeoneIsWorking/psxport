#include "run_end_report.h"

#include "config_vars.h"
#include "core.h"
#include "execution_ledger.h"
#include "game.h"
#include "game_runtime.h"
#include "lightrec_executor.h"
#include "snapshot.h"

#include <lucent/log.h>

#include <string>

namespace psx {

void RunEndReport::emit(std::uint64_t productSteps) const {
  Core &core = game_.core;
  lucent::info("runtime",
               "run complete: product_steps={} presentation_fences={}",
               productSteps,
               game_.presentation.fence());
  psx::cpu::logLedger(core.lightrecExecutor().counters(), "run-end", lucent::Level::Info);
  core.guestCallCensus().log("product loop ended");
  game_.perf.reportPresents();
  game_.presentation.reportStability();
  game_.ownerJournal.flush(core);
  game_.spu_audio.reportQueue();
  if (const std::string &path = psx::config::ramdump_path(); !path.empty()) {
    psx::debug::Snapshotter::writeRam(core, path);
  }
  core.runtime.reportRun(core);
}

} // namespace psx
