#include "cd_ready_delivery.h"

#include "bios_interrupt.h"
#include "cdc_state.h"
#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "guest_call.h"
#include "hle.h"
#include "irq_edge.h"

#include <algorithm>
#include <array>
#include <lucent/log.h>

namespace {

// The callback's second argument: no sector pointer (SLUS_008.75's callbacks read neither argument).
constexpr uint32_t kNoFirstWord = 0u;

} // namespace

namespace psx::cd {

bool readyCompletionOwed(const Core &core) {
  // The hardware gate: a masked line is not delivered, and the response stays queued until enabled.
  if ((core.game->hle.i_stat & core.game->hle.i_mask & (1u << IRQ_BIT_CD)) == 0u) {
    return false;
  }
  return core.game->cdc.currentIrqType() == CdcResponse::DataReady;
}

CdReadyDelivery deliverReadyCompletion(Core &core, std::uint32_t slot, std::uint32_t status) {
  Hle &hle = core.game->hle;
  psx::cd::CdController &controller = core.game->cdc;
  // Command responses ahead of the data-ready belong to the guest's own status polls; none is retired here.
  if (!readyCompletionOwed(core)) {
    return CdReadyDelivery::NothingOwed;
  }
  // A ready callback may issue the next request; its completion must stay owed until this one returns.
  if (!hle.canDispatchInterrupt(core)) {
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
  controller.write(psx::cd::CdcRegister::Index, psx::cd::kCdcBankData);
  controller.write(psx::cd::CdcRegister::Request, 0u);
  controller.write(psx::cd::CdcRegister::Index, static_cast<uint8_t>(savedBank));
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
  const psx::cpu::ExecutionResult result = [&] {
    const Hle::InterruptScope scope(hle, core);
    psx::hle::enterInterruptHandlerContext(&core, hle.exception_exit_buf);
    return psx::cpu::dispatchGuestNow(core, callback, status, kNoFirstWord);
  }();
  return psx::cpu::completeOrPropagate(core, result) ? CdReadyDelivery::Delivered : CdReadyDelivery::GuestExited;
}

CdcResponse acknowledgeCurrentCdResponse(Core &core, std::span<std::uint8_t> response) {
  psx::cd::CdController &controller = core.game->cdc;
  std::ranges::fill(response, std::uint8_t{0});
  const CdcResponse type = controller.currentIrqType();
  if (type == CdcResponse::None) {
    return type;
  }
  const int savedBank = controller.index;
  controller.takeCurrentResponse(response.data(), static_cast<int>(response.size()));
  // Retired before the acknowledge, which raises a fresh edge for a response queued behind this one.
  controller.irq_edge = 0;
  core.game->hle.i_stat &= ~(1u << IRQ_BIT_CD);
  controller.write(psx::cd::CdcRegister::Index, psx::cd::kCdcBankIrq);
  controller.write(psx::cd::CdcRegister::IrqFlag, static_cast<std::uint8_t>(type));
  controller.write(psx::cd::CdcRegister::Index, static_cast<std::uint8_t>(savedBank));
  // As the MMIO dispatcher latches after a guest controller write.
  core.irqStatLatch();
  core.pending_work |= Core::PW_IRQ;
  return type;
}

} // namespace psx::cd
