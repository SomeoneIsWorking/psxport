// cd_stock_read_completion.h — the completion a synchronous stock CdRead still owes its guest.
//
// THE DEFECT THIS OWNS. `cd_read_stock_sync` performs a whole CdRead from the disc image and returns, and
// it never touches the CD controller: nothing is queued, no INT1 rises, and so the framework's stand-in for
// the BIOS CD-ROM interrupt handler (`cd_ready_delivery.*`) has nothing to deliver. A guest that only waits
// on a flag is unaffected. A guest that CHAINS reads — Spyro 2 and 3 issue one CdRead, spin on a
// bytes-so-far word, and start the next read from the ready callback registered with `CdReadyCallback` —
// waits forever, because on hardware the BIOS handler runs that callback once per completed read and here
// nothing does.
//
// THE CONTRACT. For a direct runtime that declares `DeliveryOwner::GuestInterrupt` AND
// `stockReadRaisesCompletion`, every successful stock read posts exactly one data-ready response to the
// controller. That response is the completion `deliverCdReadyCompletionOnInterrupt` consumes: one response
// is one delivery, because the delivery pops it, so "exactly once per completed read" is enforced by the
// controller queue rather than by a counter this module keeps. The callback is NOT called from here; the
// handler stand-in delivers it at the interrupt, with the `readyStatus` the title declared.
//
// THE LINE MUST BE OPEN, AND WHO OPENS IT. The controller's interrupt reaches the handler stand-in only
// while I_MASK bit 2 is set (`cdReadyCompletionOwed` keeps the hardware's own gate). On hardware the retail
// `CdInit` opens that line — it reaches the BIOS interrupt-enable (`a0 = 2`). A title that replaces
// `CdInit` with a native success body (Spyro 2) therefore owes the guest that effect, and
// `armCdInterrupt` is the one implementation of it. Measured on Spyro 2: the first stock read ran with
// I_MASK 0x009 (VBlank and DMA) and the completion it queued sat in the controller unowned for the whole
// run, because nothing had ever enabled bit 2.
//
// WHO IS UNAFFECTED, byte for byte: a legacy `GameConfig` consumer, a `HostPump` title, and a
// `GuestInterrupt` title that does not opt in (Spider-Man 1 streams through its own stream owner and its
// libstr callback must not be called once per stock read). None of them reaches the controller here.
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

// Does this title owe its guest a completion for every successful stock CdRead?
[[nodiscard]] bool stockReadOwesCompletion(const Core &core);

// Owe the completion for one successful stock read of `sectors` sectors from `firstLba` at CdRead mode `mode`.
// The controller announces it once the drive would have spent the read's time (`cdc_post_data_ready_after_read`),
// so the guest sees the read in flight until then. Returns true only when the completion is owed. A read that
// moved no sectors completed nothing and owes nothing.
bool raiseStockReadCompletion(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode);

// Set I_MASK bit 2 through the device, preserving every bit the guest already enabled, and ask for an
// interrupt poll because unmasking can make a latched bit deliverable. Returns true when the bit changed.
bool armCdInterrupt(Core &core);

} // namespace psx::cd
