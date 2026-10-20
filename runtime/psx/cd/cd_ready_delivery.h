// The BIOS CD-ROM interrupt handler's CdReadyCallback call for a GuestInterruptLibcd (psxport issue 0123).
#pragma once

#include "cdc_state.h"

#include <cstdint>
#include <span>

class Core;

// What one call to the handler did with the completion it found, or refused to find.
enum class CdReadyDelivery : std::uint8_t {
  NotOwned,    // this title keeps host dispatch: the arm is not part of its contract at all
  NothingOwed, // owned, but no data-ready completion is deliverable right now
  Delivered,   // one completion was consumed and its callback ran to return
  GuestExited, // delivered, and the guest carried a typed exit out of the handler
};

// Drains the current response into `response`, acknowledges it and retires the CD line; None when none is current.
namespace psx::cd {
CdcResponse acknowledgeCurrentCdResponse(Core &core, std::span<std::uint8_t> response);

// A data-ready response is current and the CD interrupt line is asserted and unmasked.
[[nodiscard]] bool readyCompletionOwed(const Core &core);

// Runs the callback in `slot` with `a0 = status` for one owed completion; NothingOwed leaves it owed.
[[nodiscard]] CdReadyDelivery deliverReadyCompletion(Core &core, std::uint32_t slot, std::uint32_t status);
} // namespace psx::cd
