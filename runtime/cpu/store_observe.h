// store_observe — arms the executor's store observer from PSXPORT_STORE_OBSERVE and reports what it saw.
#pragma once

#include "lightrec_executor.h"

class Core;

namespace psx::cpu {

// Arms on the configured store-instruction PCs (not data addresses); an empty list disarms.
void configureStoreObserver(Core &core);

// Logs the per-PC counts; silent when the observer was never armed.
void reportStoreObserver(const StoreObserverReport &report);

} // namespace psx::cpu
