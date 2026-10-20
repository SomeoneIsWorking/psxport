// execution_ledger — the one text of every executor counter, for the run-end log and the live endpoint.
#pragma once

#include "lightrec_executor.h"

#include <lucent/log.h>

#include <string>
#include <string_view>
#include <vector>

namespace psx::cpu {

// The ledger as `name: key=value ...` lines naming every counter, so an absent name never reads as zero.
std::vector<std::string> ledgerLines(const ExecutorCounters &counters);

// One line per ledger line on the `guest` channel, prefixed `<phase>:`.
void logLedger(const ExecutorCounters &counters, std::string_view phase, lucent::Level level);

} // namespace psx::cpu
