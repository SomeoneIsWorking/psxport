// execution_ledger — the whole-run guest-execution ledger, in ONE format.
//
// The dynarec's denominators (translated and executed blocks and instructions, cache hits and misses,
// invalidations by who asked, budget exits, and interpreter fallback by every reason it counts) used to
// reach a reader only through the live debug endpoint's `guest` command, or through a destructor's
// telemetry line that a title which never destroys its Game (`new Game()` with no delete) does not
// reach. A run that ended cleanly therefore left no ledger in its default log. This module owns the
// text of the ledger, so the live endpoint and the run-end report cannot disagree about what a counter
// is called, and `logRunEndLedger` is what every native boot prints when its frame loop returns.
#pragma once

#include "lightrec_executor.h"

#include <string>
#include <vector>

namespace psx::cpu {

// The ledger as `name: key=value ...` lines: `guest`, `invalidations_by_source`,
// `invalidation_work`, `budget_exit`, `fallback`. Every counter of ExecutorCounters that the ledger reports appears by
// name, including each fallback reason and each refused-fallback reason, so an absent name is never read as a zero.
std::vector<std::string> ledgerLines(const ExecutorCounters &counters);

// One Lucent info line per ledger line, prefixed `run-end:`, on the `guest` channel.
void logRunEndLedger(const ExecutorCounters &counters);

} // namespace psx::cpu
