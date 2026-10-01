#include "cd_ready_delivery.h"

#include "cd_control.h"
#include "cdc_state.h"
#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "guest_cd_stream_callback_layout.h"
#include "hle.h"
#include "irq_edge.h"
#include "r3000.h"

#include <lucent/log.h>

namespace {

// The PlayStation CD controller's response types — the low three bits of the interrupt-flag register
// at bank 1 of 0x1F801803. 1 is the only one this handler serves; see the scope note in the header.
constexpr uint8_t kCdcIrqDataReady = 1;

// MIPS o32 argument registers for the ready-callback ABI.
enum : int { A0 = 4, A1 = 5 };

// The two arguments a stock libcd ready callback receives.
//
// `status` is the libcd completion code, not the controller's raw status byte. It defaults to 1 (a sector
// is ready) because that is what BOTH existing delivery sites in this framework already pass —
// `Cd::pumpStream` and `cd_drive_stock_read` — and a title that saw two different argument shapes
// from the same slot depending on who called it would be a worse defect than a title that sees a
// constant. `first_word` is a pointer to the sector's first word on hardware; the layout declares the
// slot and not the guest's private buffer, so this passes 0, meaning "read the sector through the
// controller", which is what the DMA the callback starts does. (Measured from SLUS_008.75's own
// bytes: the two functions its CdInit installs into this slot, and libstr's replacement for it
// during a stream, read NEITHER argument.)
constexpr uint32_t kNoFirstWord = 0u;

const GuestCdStreamCallbackLayout *declaredLayout(const Core &core) {
  const GameRuntime *runtime = core.game ? core.game->runtime : nullptr;
  return runtime ? runtime->guestCdStreamCallbackLayout() : nullptr;
}

// Read and drop the current response the way the handler that owns this interrupt does before it
// notifies anybody. Returns the response's first byte (its status). Acknowledging is a SEPARATE
// step, in the caller, so a caller that cannot deliver leaves the response in place.
uint8_t consumeCurrentResponse(CdcState &controller) {
  uint8_t first = 0;
  cdc_take_current_response(&controller, &first);
  return first;
}

} // namespace

bool cdReadyCallbackOwnedByGuestInterrupt(const Core &core) {
  // A legacy `GameConfig` consumer has no declaration to honour and keeps host dispatch: that is the
  // gate that makes this arm and `Cd::pumpStream` mutually exclusive rather than additive.
  if (core.cfg) {
    return false;
  }
  const GuestCdStreamCallbackLayout *layout = declaredLayout(core);
  return layout != nullptr && layout->valid() &&
         layout->owner == GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt;
}

bool cdReadyCompletionOwed(const Core &core) {
  if (!cdReadyCallbackOwnedByGuestInterrupt(core)) {
    return false;
  }
  // The hardware's own gate, unchanged: a masked-off interrupt is not delivered, and the response
  // stays in the controller's queue until the guest enables it.
  //
  // Note the difference from `irq_edge.h`'s rule, which is easy to misread as forbidding the clear
  // below. That rule is about the SOURCE: a hardware line must never clear a bit the guest has not
  // acknowledged, or it re-asserts immediately after an ack. What the delivery does is the
  // HANDLER's acknowledge, which is the same thing `dma_irq_ack` does for a DMA completion's DICR
  // flag. Without it the CD line stays asserted for the rest of the run, the pending word never
  // drains, and the whole delivery path is re-entered at every guest function entry.
  if ((core.game->hle.i_stat & core.game->hle.i_mask & (1u << IRQ_BIT_CD)) == 0u) {
    return false;
  }
  return cdc_current_irq_type(&core.game->cdc) == kCdcIrqDataReady;
}

CdReadyDelivery deliverCdReadyCompletionOnInterrupt(Core &core) {
  Hle &hle = core.game->hle;
  if (!cdReadyCallbackOwnedByGuestInterrupt(core)) {
    return CdReadyDelivery::NotOwned;
  }
  CdcState &controller = core.game->cdc;
  // Retire the responses of commands the FRAMEWORK issued on the guest's behalf (the synchronous
  // command owner answers the guest before the controller can, so those INT3/INT2 responses have no
  // guest consumer). Hardware serves the response FIFO in order, so leaving them current hides
  // every data-ready completion queued behind one — which is exactly what a title whose libcd
  // command leaf is native sees: its read completes once and then never completes again.
  int retired = 0;
  while (cdc_take_owed_command_response(&controller)) {
    ++retired;
  }
  if (retired) {
    hle.i_stat &= ~(1u << IRQ_BIT_CD);
    const int bank = controller.index;
    cdc_write(&controller, CDC_REG_INDEX, CDC_BANK_IRQ);
    cdc_write(&controller, CDC_REG_IRQ_FLAG, 1u);
    cdc_write(&controller, CDC_REG_INDEX, static_cast<uint8_t>(bank));
    core.irqStatLatch();
  }
  if (!cdReadyCompletionOwed(core)) {
    return CdReadyDelivery::NothingOwed;
  }
  // Re-entrancy: a guest ready callback may issue the next CD request, whose response becomes
  // current before the callback returns, and the delivery must find that completion owed rather
  // than already taken. This is the same refusal the DMA arm makes above it, for the same reason,
  // and for the same measured consequence — these completions chain.
  if (hle.in_irq || !hle.canDispatchInterrupt(core)) {
    hle.cd_ready_declined++;
    lucent::debug("cdirq",
                  "CD data-ready owed, DEFERRED (a guest interrupt handler is running) — {} of {} "
                  "owed completions delivered so far",
                  hle.cd_ready_delivered,
                  hle.cd_ready_delivered + hle.cd_ready_declined);
    return CdReadyDelivery::NothingOwed;
  }
  // The slot is re-read HERE, not cached: a stock libcd owner replaces the function during a stream
  // (SLUS_008.75 installs 0x8008A260 at CdInit and libstr replaces it with 0x800860B4), so a
  // delivery that used the value it saw at stream start would call a callback the guest has since
  // uninstalled. Nothing installed means nothing to deliver, and the completion stays owed for
  // whoever installs one.
  const uint32_t slot = cd_ready_callback_pointer(core);
  const uint32_t callback = slot ? core.mem_r32(slot) : 0u;
  if (!callback) {
    hle.cd_ready_declined++;
    lucent::debug("cdirq",
                  "CD data-ready owed, NOTHING DELIVERED: slot 0x{:08X} holds 0x{:08X} — the "
                  "completion stays owed ({} of {} owed completions delivered)",
                  slot,
                  callback,
                  hle.cd_ready_delivered,
                  hle.cd_ready_delivered + hle.cd_ready_declined);
    return CdReadyDelivery::NothingOwed;
  }

  // Declared by the title (`readyStatus`, default 1); read from its callback's own bytes, never guessed here.
  const uint32_t status = declaredLayout(core)->readyStatus;
  const int savedBank = controller.index;
  const uint8_t responseStatus = consumeCurrentResponse(controller);
  // This handler has now serviced the CD interrupt, so its line is acknowledged. Done BEFORE the
  // controller acknowledge, because that acknowledge is what can raise a fresh edge for a response
  // already queued behind this one.
  hle.i_stat &= ~(1u << IRQ_BIT_CD);
  cdc_write(&controller, CDC_REG_INDEX, CDC_BANK_IRQ);
  cdc_write(&controller, CDC_REG_IRQ_FLAG, 1u);
  cdc_write(&controller, CDC_REG_INDEX, static_cast<uint8_t>(savedBank));
  // Fold an edge that acknowledge just raised, so a response queued behind this one is visible to
  // the next poll instead of waiting for an unrelated I_STAT access. This is the same
  // `irqStatLatch()` the MMIO dispatcher performs after a guest controller write.
  core.irqStatLatch();
  core.pending_work |= Core::PW_IRQ;
  hle.cd_ready_delivered++;

  lucent::debug("cdirq",
                "CD data-ready -> callback 0x{:08X} (slot 0x{:08X}, controller status 0x{:02X}, "
                "a0={} a1={}) — {} of {} owed completions delivered",
                callback,
                slot,
                responseStatus,
                status,
                kNoFirstWord,
                hle.cd_ready_delivered,
                hle.cd_ready_delivered + hle.cd_ready_declined);

  // The callback runs as an ordinary guest function with the whole register context saved and
  // restored around it, exactly as an exception entry would, so the interrupted context sees
  // nothing — the same contract the DMA arm above and `cd_drive_stock_read` both state.
  hle.in_irq = 1; // the callback's own CD calls must not re-enter this delivery
  const R3000 saved = *static_cast<R3000 *>(&core);
  Hle::enterExceptionStack(core);
  core.r[A0] = status;
  core.r[A1] = kNoFirstWord;
  const auto result = psx::cpu::dispatchGuest0(core, callback, psx::cpu::ExecutionBudget::currentTurn(core));
  *static_cast<R3000 *>(&core) = saved;
  hle.in_irq = 0;
  // The completion WAS delivered, so a typed exit out of the handler is not a reason to withhold it —
  // but the caller must stop, exactly as the DMA arm stops on the same event.
  return psx::cpu::completeOrPropagate(core, result) ? CdReadyDelivery::Delivered : CdReadyDelivery::GuestExited;
}
