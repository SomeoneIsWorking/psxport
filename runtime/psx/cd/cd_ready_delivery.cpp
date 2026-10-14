#include "cd_ready_delivery.h"

#include "bios_interrupt.h"
#include "cdc_state.h"
#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "guest_call.h"
#include "hle.h"
#include "irq_edge.h"
#include "r3000.h"

#include <algorithm>
#include <array>
#include <lucent/log.h>

namespace {

// The PlayStation CD controller's response types — the low three bits of the interrupt-flag register
// at bank 1 of 0x1F801803; this handler serves data-ready only.
constexpr uint8_t kCdcIrqDataReady = 1;

// MIPS o32 argument registers for the ready-callback ABI.
enum : int { A0 = 4, A1 = 5 };

// The callback's second argument: no sector pointer, the callback reads the sector through the drive
// (SLUS_008.75's callbacks read neither argument).
constexpr uint32_t kNoFirstWord = 0u;

} // namespace

namespace psx::cd {

bool readyCompletionOwed(const Core &core) {
  // The hardware gate: a masked line is not delivered, and the response stays queued until enabled.
  if ((core.game->hle.i_stat & core.game->hle.i_mask & (1u << IRQ_BIT_CD)) == 0u) {
    return false;
  }
  return cdc_current_irq_type(&core.game->cdc) == kCdcIrqDataReady;
}

CdReadyDelivery deliverReadyCompletion(Core &core, std::uint32_t slot, std::uint32_t status) {
  Hle &hle = core.game->hle;
  CdcState &controller = core.game->cdc;
  // Command responses ahead of the data-ready belong to the guest's own status polls; none is retired here.
  if (!readyCompletionOwed(core)) {
    return CdReadyDelivery::NothingOwed;
  }
  // A ready callback may issue the next request; its completion must stay owed until this one returns.
  if (hle.in_irq || !hle.canDispatchInterrupt(core)) {
    hle.cd_ready_declined++;
    lucent::debug("cdirq",
                  "CD data-ready owed, deferred (a guest interrupt handler is running); {} of {} delivered",
                  hle.cd_ready_delivered,
                  hle.cd_ready_delivered + hle.cd_ready_declined);
    return CdReadyDelivery::NothingOwed;
  }
  // Read the slot now: libstr replaces the callback during a stream (SLUS_008.75: 0x8008A260 -> 0x800860B4).
  const uint32_t callback = slot ? core.mem_r32(slot) : 0u;
  if (!callback) {
    hle.cd_ready_declined++;
    lucent::debug("cdirq",
                  "CD data-ready owed, nothing installed: slot 0x{:08X} holds 0x{:08X} ({} of {} delivered)",
                  slot,
                  callback,
                  hle.cd_ready_delivered,
                  hle.cd_ready_delivered + hle.cd_ready_declined);
    return CdReadyDelivery::NothingOwed;
  }
  std::array<uint8_t, 1> response{};
  acknowledgeCurrentCdResponse(core, response);
  // Stock libcd's INT1 handler clears the request register before the callback, emptying the data FIFO.
  const int savedBank = controller.index;
  cdc_write(&controller, CDC_REG_INDEX, CDC_BANK_DATA);
  cdc_write(&controller, CDC_REG_REQUEST, 0u);
  cdc_write(&controller, CDC_REG_INDEX, static_cast<uint8_t>(savedBank));
  hle.cd_ready_delivered++;
  lucent::debug("cdirq",
                "CD data-ready -> callback 0x{:08X} (slot 0x{:08X}, controller status 0x{:02X}, a0={} a1={}); {} of "
                "{} delivered",
                callback,
                slot,
                response[0],
                status,
                kNoFirstWord,
                hle.cd_ready_delivered,
                hle.cd_ready_delivered + hle.cd_ready_declined);
  // An interrupt handler's context: the whole register file restored, and in_irq held for the callback.
  hle.in_irq = 1;
  const R3000 saved = *static_cast<R3000 *>(&core);
  if (!bios_interrupt_enter_handler_context(&core, hle.exception_exit_buf)) {
    Hle::enterExceptionStack(core);
  }
  core.r[A0] = status;
  core.r[A1] = kNoFirstWord;
  const auto result = psx::cpu::dispatchGuest0(core, callback, psx::cpu::ExecutionBudget::currentTurn(core));
  *static_cast<R3000 *>(&core) = saved;
  hle.in_irq = 0;
  return psx::cpu::completeOrPropagate(core, result) ? CdReadyDelivery::Delivered : CdReadyDelivery::GuestExited;
}

std::uint8_t acknowledgeCurrentCdResponse(Core &core, std::span<std::uint8_t> response) {
  CdcState &controller = core.game->cdc;
  std::ranges::fill(response, std::uint8_t{0});
  const std::uint8_t type = cdc_current_irq_type(&controller);
  if (type == 0u) {
    return 0u;
  }
  const int savedBank = controller.index;
  cdc_take_current_response(&controller, response.data(), static_cast<int>(response.size()));
  // The line is retired BEFORE the controller acknowledge, because that acknowledge is what raises a
  // fresh edge for a response already queued behind this one.
  controller.irq_edge = 0;
  core.game->hle.i_stat &= ~(1u << IRQ_BIT_CD);
  cdc_write(&controller, CDC_REG_INDEX, CDC_BANK_IRQ);
  cdc_write(&controller, CDC_REG_IRQ_FLAG, type);
  cdc_write(&controller, CDC_REG_INDEX, static_cast<std::uint8_t>(savedBank));
  // The same `irqStatLatch()` the MMIO dispatcher performs after a guest controller write, so the
  // next response is visible to the next poll.
  core.irqStatLatch();
  core.pending_work |= Core::PW_IRQ;
  return type;
}

} // namespace psx::cd
