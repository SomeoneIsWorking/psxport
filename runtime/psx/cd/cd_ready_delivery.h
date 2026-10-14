// cd_ready_delivery.h — the framework's stand-in for the BIOS CD-ROM interrupt handler.
//
// A stock libcd learns a sector arrived because the BIOS handler calls the CdReadyCallback pointer the
// guest installed; no event class is involved (psxport issue 0123). For a psx::cd::GuestInterruptLibcd
// the framework plays that handler: read the controller's data-ready response, acknowledge it and call
// the slot's current value. It runs only after the guest's own interrupt elements declined the CD bit,
// and never for a host-pumped libcd, which gets its one callback per completion from the pump.
// A completion is consumed only when it is delivered, because these completions chain.
#pragma once

#include <cstdint>
#include <span>

class Core;

// What one call to the handler did with the completion it found, or refused to find.
enum class CdReadyDelivery {
  NotOwned,    // this title keeps host dispatch: the arm is not part of its contract at all
  NothingOwed, // owned, but no data-ready completion is deliverable right now
  Delivered,   // one completion was consumed and its callback ran to return
  GuestExited, // delivered, and the guest carried a typed exit out of the handler
};

// The handler's service of the CURRENT controller response, whatever its type: drain its bytes into
// `response` (the rest zeroed), retire the CD line, acknowledge the controller and fold the edge that
// acknowledge raises for a response queued behind it. Returns the response type, 0 when none is
// current. The one owner of that sequence; a title that serves libcd's CdReady natively calls it too.
namespace psx::cd {
std::uint8_t acknowledgeCurrentCdResponse(Core &core, std::span<std::uint8_t> response);

// A data-ready response is current and the CD interrupt line is asserted and unmasked.
[[nodiscard]] bool readyCompletionOwed(const Core &core);

// Delivers at most one owed data-ready completion to the callback in `slot`, with `a0 = status`. Every
// `NothingOwed` leaves the completion owed; the register context is restored on every path.
[[nodiscard]] CdReadyDelivery deliverReadyCompletion(Core &core, std::uint32_t slot, std::uint32_t status);
} // namespace psx::cd
