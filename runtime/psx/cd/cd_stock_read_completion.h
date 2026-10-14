// cd_stock_read_completion.h — the completion a synchronous stock CdRead still owes its guest.
//
// StockLibcd::read lands the whole read without touching the controller, so no INT1 rises. A guest that
// chains reads from its ready callback (Spyro 2 and 3) waits for one, so a GuestInterruptLibcd that
// declares `perStockRead` queues exactly one data-ready response per successful read, and
// cd_ready_delivery delivers it with the title's status. The line must be open: a title that replaces
// CdInit natively owes the guest the I_MASK bit 2 retail CdInit sets, through `armCdInterrupt`.
#pragma once

#include <cstdint>

class Core;

namespace psx::cd {

// One whole synchronous stock `CdRead` after its bytes are visible in guest RAM: where they came from,
// where they landed, and how many. `destination` is the guest address the read was issued with (as the
// guest wrote it, so a KSEG0 pointer stays one); `bytes` is exactly what was written there, sector
// payload size included, never the requested-sector count times a constant the caller assumed.
struct StockReadLanding {
  std::uint32_t firstLba = 0;
  std::uint32_t sectors = 0;
  std::uint32_t destination = 0;
  std::uint32_t bytes = 0;
};

// Tell the direct runtime that a stock read landed, once per whole successful read, after every byte is
// written and reported to the invalidation owner and before the read's completion is queued. A read that
// moved no bytes, or failed part way, announces nothing: nothing has landed that a runtime could name.
// The runtime (`GameRuntime::stockCdReadLanded`) owns what a landing means to its title, for instance
// publishing the bytes as an authenticated code image; the framework owns only that it happened.
void announceStockReadLanding(Core &core, const StockReadLanding &landing);

// Owes the completion for one successful stock read of `sectors` sectors from `firstLba` at CdRead mode `mode`.
// The controller announces it once the drive would have spent the read's time (`cdc_post_data_ready_after_read`),
// so the guest sees the read in flight until then. Returns true only when the completion is owed. A read that
// moved no sectors completed nothing and owes nothing.
bool raiseStockReadCompletion(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode);

// Set I_MASK bit 2 through the device, preserving every bit the guest already enabled, and ask for an
// interrupt poll because unmasking can make a latched bit deliverable. Returns true when the bit changed.
bool armCdInterrupt(Core &core);

} // namespace psx::cd
