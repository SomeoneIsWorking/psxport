// The data-ready completion a synchronous stock CdRead owes a guest that chains reads from its callback.
#pragma once

#include <cstdint>

class Core;

namespace psx::cd {

// One landed stock CdRead; `destination` is the address as the guest issued it, `bytes` what was written.
struct StockReadLanding {
  std::uint32_t firstLba = 0;
  std::uint32_t sectors = 0;
  std::uint32_t destination = 0;
  std::uint32_t bytes = 0;
};

// Calls GameRuntime::stockCdReadLanded once per whole successful read, before its completion is queued.
void announceStockReadLanding(Core &core, const StockReadLanding &landing);

// Queues the completion once the drive would have spent the read's time; false when none is owed.
bool raiseStockReadCompletion(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode);

// Sets I_MASK bit 2 as retail CdInit does, for a title that replaces CdInit; true when the bit changed.
bool armCdInterrupt(Core &core);

} // namespace psx::cd
