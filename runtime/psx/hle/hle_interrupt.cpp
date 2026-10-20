// BIOS interrupt-chain registration and delivery: DMA completion callbacks, the SysEnqIntRP chain,
// libcd's own CD handler and the HookEntryInt continuation.
#include "bios_interrupt.h"
#include "cd_ready_delivery.h"
#include "core.h"
#include "dma_callbacks.h"
#include "dma_irq.h"
#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "hle.h"
#include "irq_edge.h"
#include "stock_dma_callbacks.h"
#include "stock_libcd.h"

#include <cstdlib>
#include <lucent/log.h>

namespace {

void dispatchCustomExceptionExit(Core *core, uint32_t address) {
  psx::cpu::callGuestNow(*core, "BIOS custom exception exit", address);
}

// A guest-supplied word that names no loaded image faults when dispatched.
void reportUnloadedEntry(Core &core, const char *slot, uint32_t elem, uint32_t entry, uint32_t at) {
  if (core.imageCatalog().resolve(entry).has_value()) {
    return;
  }
  lucent::error("irq",
                "interrupt element 0x{:08X} has {} 0x{:08X} at [0x{:08X}] in no loaded code image; mask=0x{:08X}",
                elem,
                slot,
                entry,
                at,
                core.mem_r32(elem));
}

} // namespace

void Hle::enterExceptionStack(Core &core) {
  core.r[psx::cpu::kSp] = kExceptionStackTop;
  core.r[psx::cpu::kRa] = kInterruptReturnSentinel;
}

Hle::InterruptScope::InterruptScope(Hle &hle, Core &core) : hle_(hle), context_(core), nested_(hle.in_irq != 0) {
  hle_.in_irq = 1;
  if (!nested_) {
    enterExceptionStack(core);
  }
}

Hle::InterruptScope::~InterruptScope() {
  hle_.in_irq = nested_ ? 1 : 0;
}

psx::cpu::ExecutionResult Hle::runAsInterrupt(Core &core, uint32_t entry) {
  const InterruptScope scope(*this, core);
  return psx::cpu::dispatchGuestNow(core, entry);
}

// Priority-ordered and idempotent: guests re-register with SysDeqIntRP then SysEnqIntRP.
void Hle::irqEnq(uint32_t prio, uint32_t elem) {
  if (!elem) {
    return;
  }
  irqDeq(elem);
  if (irq_n >= IRQ_CHAIN_MAX) {
    lucent::warn("irq",
                 "interrupt chain full ({}) — dropping elem 0x{:08X}. Raise IRQ_CHAIN_MAX; a "
                 "silently dropped handler is an interrupt that never gets serviced.",
                 (int)IRQ_CHAIN_MAX,
                 elem);
    return;
  }
  int at = irq_n;
  while (at > 0 && irq_prio[at - 1] > prio) { // lower priority value runs first
    irq_elem[at] = irq_elem[at - 1];
    irq_prio[at] = irq_prio[at - 1];
    at--;
  }
  irq_elem[at] = elem;
  irq_prio[at] = prio;
  irq_n++;
  lucent::info("irq", "registered interrupt element 0x{:08X} prio={} (chain now {})", elem, prio, irq_n);
}

void Hle::irqDeq(uint32_t elem) {
  for (int i = 0; i < irq_n; i++) {
    if (irq_elem[i] != elem) {
      continue;
    }
    for (int j = i; j + 1 < irq_n; j++) {
      irq_elem[j] = irq_elem[j + 1];
      irq_prio[j] = irq_prio[j + 1];
    }
    irq_n--;
    return;
  }
}

bool Hle::canDispatchInterrupt(const Core &core) const {
  return irq_enabled && !in_irq && !core.active_native_address && !core.pending_guest_redirect;
}

bool Hle::irqDeliverable(Core &core, bool cdDeliveryPath) const {
  return core.irqPendingEnabled() != 0 && (irq_n != 0 || exception_exit_buf != 0 || cdDeliveryPath);
}

void Hle::disarmIrqGateIfIdle(Core &core, bool cdDeliveryPath) const {
  if (!core.dma.done.any() && !irqDeliverable(core, cdDeliveryPath)) {
    core.pending_work &= ~Core::PW_IRQ;
  }
}

void Hle::countDecline(const Core &core) {
  if (!lucent::channel_on("irq")) {
    return;
  }
  if (!irq_enabled) {
    irqDeclines_.critical++;
  } else if (in_irq) {
    irqDeclines_.nested++;
  } else {
    irqDeclines_.transient++;
  }
  const unsigned total = irqDeclines_.critical + irqDeclines_.nested + irqDeclines_.transient;
  if (total % 200000 == 0 && irqDeclines_.reports < 6) {
    irqDeclines_.reports++;
    lucent::info("irq",
                 "delivery declined {} times: critical-section={} nested={} transient={} (irq_enabled={} now)",
                 total,
                 irqDeclines_.critical,
                 irqDeclines_.nested,
                 irqDeclines_.transient,
                 irq_enabled);
  }
}

// The BIOS order: owed DMA callbacks, then the chain (verifier, then the claiming handler), then libcd's CD
// handler for a CD bit left unacknowledged, then the custom exit. GTE state is not saved, as on hardware.
void Hle::irqPoll(Core *c) {
  const psx::hle::StockDmaCallbacks *guestTable = c->game->dmaCallbacks.guestTable();
  for (unsigned index = 0; index < psx::dma::kChannelCount; index++) {
    const auto ch = static_cast<psx::dma::DmaChannel>(index);
    if (!c->dma.done.owed(ch)) {
      continue;
    }
    const uint32_t cb = guestTable ? guestTable->callback(*c, ch) : c->game->dmaCallbacks.current(ch);
    lucent::debug("dmairq",
                  "owed ch{} -> callback {:08X} ({}){}",
                  index,
                  cb,
                  guestTable ? "guest table" : "direct registry",
                  in_irq ? "  DEFERRED: a guest callback is running" : "");
    // Consumed only when delivered: a callback can start the transfer whose completion it then owes.
    if (in_irq) {
      continue;
    }
    if (guestTable && !psx::hle::StockDmaCallbacks::handlerRuns(*c)) {
      continue;
    }
    c->dma.done.taken(ch);
    c->dma.acknowledge(ch);
    if (!cb) {
      continue;
    }
    if (!psx::cpu::completeOrPropagate(*c, runAsInterrupt(*c, cb))) {
      return;
    }
  }

  if (in_irq || !irq_enabled) {
    countDecline(*c);
    return;
  }
  const psx::cd::StockLibcd *libcd = c->game->cd.stockLibrary();
  const bool cd_delivery_path = libcd != nullptr && libcd->deliversOnInterrupt();
  // The gate stays armed while a DMA completion is owed, so a chained transfer is not lost.
  if (!irqDeliverable(*c, cd_delivery_path)) {
    disarmIrqGateIfIdle(*c, cd_delivery_path);
    return;
  }
  if (!canDispatchInterrupt(*c)) {
    countDecline(*c);
    return;
  }
  const uint32_t pending = c->irqPendingEnabled();

  int claimed = 0;
  {
    const InterruptScope scope(*this, *c);
    for (int i = 0; i < irq_n && !claimed; i++) {
      const uint32_t elem = irq_elem[i];
      const uint32_t handler = c->mem_r32(elem + 4);
      const uint32_t verifier = c->mem_r32(elem + 8);
      if (verifier) {
        reportUnloadedEntry(*c, "verifier", elem, verifier, elem + 8);
        const auto result = psx::cpu::dispatchGuestNow(*c, verifier);
        if (!psx::cpu::completeOrPropagate(*c, result)) {
          return;
        }
        if (c->r[psx::cpu::kV0] == 0) {
          continue;
        }
      }
      if (!handler) {
        continue;
      }
      // The BIOS passes the verifier's return to the handler in a0.
      c->r[psx::cpu::kA0] = c->r[psx::cpu::kV0];
      reportUnloadedEntry(*c, "handler", elem, handler, elem + 4);
      lucent::debug(
          "irq", "delivering: elem 0x{:08X} handler 0x{:08X} (I_STAT&I_MASK=0x{:03X})", elem, handler, pending);
      if (!psx::cpu::completeOrPropagate(*c, psx::cpu::dispatchGuestNow(*c, handler))) {
        return;
      }
      claimed = 1;
    }
  }
  // No registered element claimed it; libcd or the custom exit may still own the source.
  if (!claimed && pending != lastUnclaimedIrq_) {
    lastUnclaimedIrq_ = pending;
    lucent::info("irq",
                 "pending I_STAT&I_MASK=0x{:03X}; no SysEnq element claimed it ({} in chain), custom exception exit {}",
                 pending,
                 irq_n,
                 exception_exit_buf ? "installed" : "not installed");
  }

  // A claiming verifier names one source; a CD bit its handler left in I_STAT is still owed. The CD
  // handler is its own interrupt and restores the register file itself.
  if (cd_delivery_path && (!claimed || c->irqPendingEnabled(IRQ_BIT_CD))) {
    if (libcd->deliverOnInterrupt(*c) == CdReadyDelivery::GuestExited) {
      return;
    }
  }

  if (exception_exit_buf) {
    const InterruptScope scope(*this, *c);
    c->r[psx::cpu::kRa] = kInterruptReturnSentinel;
    custom_exit_active = 1;
    const BiosInterruptDispatchResult result =
        psx::hle::dispatchInterruptCustomExit(c, exception_exit_buf, dispatchCustomExceptionExit);
    custom_exit_active = 0;
    if (result != BiosInterruptDispatchResult::ReturnedFromException) {
      lucent::error("irq", "custom exception exit violated B0:0x17 contract (result={})", (int)result);
      abort();
    }
  }
  disarmIrqGateIfIdle(*c, cd_delivery_path);
}
