// BIOS interrupt-chain registration and delivery, including DMA completion service and
// the custom HookEntryInt continuation. State and the public API remain on the per-Game Hle.
#include "bios_interrupt.h"
#include "cd_ready_delivery.h"
#include "core.h"
#include "dma_callbacks.h"
#include "dma_irq.h"
#include "execution_control.h"
#include "game.h"
#include "guest_call.h"
#include "hle.h"

#include <cstdlib>
#include <lucent/log.h>

namespace {

// MIPS o32 argument and result registers used by the BIOS interrupt-chain ABI.
enum { A0 = 4, V0 = 2 };

static void dispatchCustomExceptionExit(Core *core, uint32_t address) {
  psx::cpu::dispatchGuestToReturn0(
      *core, address, psx::cpu::ExecutionBudget::currentTurn(*core), "BIOS custom exception exit");
}

} // namespace

// ---- interrupt delivery -------------------------------------------------------------------------
// Registering an element is priority-ordered and idempotent: the standard guest idiom is
// SysDeqIntRP(prio, elem) immediately followed by SysEnqIntRP(prio, elem), so a re-register must
// replace rather than accumulate.
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

// Deliver ONE pending interrupt to the guest's chain, exactly as the BIOS exception path would:
// walk in priority order, run each element's VERIFIER, and on the first that claims the interrupt
// run that element's HANDLER and stop.
//
// The runtime reaches this service with the guest register file synchronized into Core. Saving
// R3000 therefore preserves the complete architectural GPR/HI/LO/PC state; nested guest dispatch
// returns through that same synchronized boundary before the interrupted context is restored.
// Guest MEMORY changes are deliberately NOT undone — mutating memory is what an ISR is for.
//
// GTE/cop2 state is deliberately NOT saved: real hardware does not save it on exception entry
// either, so an ISR that clobbers it must save it itself. Matching the hardware is the faithful
// choice; saving it here would mask a genuinely misbehaving guest handler.
void Hle::irqPoll(Core *c) {
  // A completed CD DMA owes the guest its completion callback. This is genuinely interrupt-shaped —
  // on hardware the transfer raises an IRQ and the BIOS runs the handler — so it is serviced here,
  // at the same safe boundary, rather than from the store that finished the transfer.
  // EVERY channel, lowest first, exactly as the BIOS handler scans them. It used to be channel 3
  // alone, and that hid a whole subsystem: Spider-Man's FMV player registers an MDEC-out (channel 1)
  // callback that uploads each decoded strip to VRAM, and it was never called once in a run.
  const uint32_t table = c->cfg ? c->cfg->dmaCallbackTable : 0;
  for (int ch = 0; ch < 7; ch++) {
    if (!dma_done_owed(ch)) {
      continue;
    }
    const uint32_t slot = dma_callback_slot(table, ch);
    const uint32_t cb =
        c->cfg ? (slot ? c->mem_r32(slot) : 0) : c->game->dmaCallbacks.current(static_cast<DmaChannel>(ch));
    lucent::debug("dmairq",
                  "owed ch{} -> callback {:08X} ({} {:08X}){}",
                  ch,
                  cb,
                  c->cfg ? "guest slot" : "direct registry",
                  slot,
                  in_irq ? "  DEFERRED: a guest callback is running" : "");
    // A COMPLETION IS ONLY CONSUMED WHEN IT IS DELIVERED. Taking it first and then declining the
    // dispatch silently DROPS the callback, and that is not a corner case: these callbacks CHAIN.
    // The FMV player's MDEC-out handler starts the next strip's transfer, which finishes while the
    // handler is still running — so the second completion always lands with `in_irq` set. Consuming
    // it there ended the chain after two strips per movie, with the decoder left holding 1640
    // abandoned input words. Left owed, the next poll delivers it and the chain runs to the end.
    if (in_irq) {
      continue;
    }
    dma_done_taken(ch);
    dma_irq_ack(ch); // this dispatch stands in for the BIOS DMA handler, which acknowledges first
    if (!cb) {
      continue; // nothing registered: the completion is consumed
    }
    in_irq = 1; // the callback's own BIOS calls must not re-enter
    const R3000 saved = *static_cast<R3000 *>(c);
    const auto result = psx::cpu::dispatchGuest0(*c, cb, psx::cpu::ExecutionBudget::currentTurn(*c));
    *static_cast<R3000 *>(c) = saved;
    in_irq = 0;
    if (!psx::cpu::completeOrPropagate(*c, result)) {
      return;
    }
  }

  // Decline accounting. A ONE-SHOT version of this could not distinguish "declined once early" from
  // "declined for the entire run", which are completely different diagnoses — so count each reason
  // and report periodically. Cheap: only runs when the channel is on.
  if (lucent::channel_on("irq")) {
    static unsigned n_crit = 0, n_nest = 0, n_transient = 0, n_report = 0;
    if (!irq_enabled) {
      n_crit++;
    } else if (in_irq) {
      n_nest++;
    } else if (c->active_native_address || c->pending_guest_redirect) {
      n_transient++;
    }
    const unsigned tot = n_crit + n_nest + n_transient;
    if (tot && (tot % 200000) == 0 && n_report < 6) {
      n_report++;
      lucent::info("irq",
                   "delivery declined {} times: critical-section={} nested={} transient={} "
                   "(irq_enabled={} now)",
                   tot,
                   n_crit,
                   n_nest,
                   n_transient,
                   irq_enabled);
    }
  }
  if (in_irq || !irq_enabled) {
    return;
  }
  // The framework's own CD-ROM interrupt handler is a delivery path in its own right, so a title
  // that registers NO SysEnq element and installs NO custom exception exit still reaches it. It
  // decides for itself whether a completion is owed, and it only runs after the chain below has
  // declined the interrupt — so counting it here cannot deliver anything twice.
  const bool cd_delivery_path = cdReadyCallbackOwnedByGuestInterrupt(*c);
  const uint32_t pending = c->irqStatLatch() & i_mask;
  // Clear the gate whenever there is nothing to deliver, so the common case costs one load-and-test
  // per function entry and nothing more. Re-armed by whoever raises next.
  // A DMA completion recorded DURING the callback dispatch above (the FMV player's MDEC-out handler
  // starts the NEXT strip's transfer, which can finish before it returns) leaves a channel still
  // owed. Clearing the gate then loses it, and the chain stops after one strip — measured: two
  // strips per movie decoded and then "no decode command in flight". So the gate survives while
  // anything is owed, and only the genuinely-idle case pays nothing.
  const bool has_delivery_path = irq_n != 0 || exception_exit_buf != 0 || cd_delivery_path;
  if ((!pending || !has_delivery_path) && !dma_done_any()) {
    c->pending_work &= ~Core::PW_IRQ;
    return;
  }
  if (!pending || !has_delivery_path) {
    return;
  }

  // These two are transient per-Core execution state consumed by the dispatch machinery. If either
  // is live we are NOT at a clean boundary, and delivering here could lose a pending redirect.
  if (!canDispatchInterrupt(*c)) {
    return;
  }

  in_irq = 1;
  int claimed = 0;
  R3000 saved = *static_cast<R3000 *>(c); // r[0..31] + hi + lo + pc — the whole guest context

  for (int i = 0; i < irq_n; i++) {
    const uint32_t elem = irq_elem[i];
    const uint32_t handler = c->mem_r32(elem + 4);
    const uint32_t verifier = c->mem_r32(elem + 8);
    if (verifier) {
      // THE VERIFIER IS GUARDED TOO, AND IT IS THE ONE THAT FAULTS. The guard below covers the
      // HANDLER at elem+4; on Mega Man X4 it reported nothing, all run, while the port still faulted
      // with a dispatch of 0x0113D7D0 - because this loop dispatches TWO words, and the VERIFIER at
      // elem+8 is dispatched FIRST and was unguarded. A diagnostic on one of two sites is the same
      // partial coverage as a census on one of sixteen classes, and it produced the same confident
      // zero. Both words a guest registers are now checked, because the guest supplies the element.
      if (!c->currentImageIdentity(verifier).has_value()) {
        lucent::error("irq",
                      "interrupt element 0x{:08X} has VERIFIER 0x{:08X} at [0x{:08X}], which is in "
                      "NO loaded code image; dispatching it would fault. handler=0x{:08X} mask=0x{:08X}",
                      elem,
                      verifier,
                      elem + 8,
                      handler,
                      c->mem_r32(elem));
      }
      const auto result = psx::cpu::dispatchGuest0(*c, verifier, psx::cpu::ExecutionBudget::currentTurn(*c));
      if (!result.returned()) {
        *static_cast<R3000 *>(c) = saved;
        in_irq = 0;
        psx::cpu::completeOrPropagate(*c, result);
        return;
      }
      if (c->r[V0] == 0) {
        continue; // not this element's interrupt
      }
    }
    if (!handler) {
      continue;
    }
    // The BIOS passes the verifier's return to the handler; a handler that reads $a0 expects it.
    c->r[A0] = c->r[V0];
    // A HANDLER THAT IS NOT EXECUTABLE IS REPORTED HERE, WITH THE ELEMENT IT CAME FROM.
    //
    // This is the delivery site the caller-naming diagnostic pointed at on Mega Man X4: the port
    // faulted with a dispatch of 0x0113D7D0, `dispatchGuest` was HANDED that address, and this loop
    // is the only live path that dispatches a MEMORY-READ handler. The element is a guest
    // InterruptElement the guest registered through the BIOS (`irqEnq(a0, a1)` takes the address
    // from the guest), so the address is not derivable from any table a title can name - only this
    // scope knows it. Measured on MMX4: exactly ONE element is ever registered, 0x8013BBF8, so its
    // handler word is [0x8013BBFC].
    if (!c->currentImageIdentity(handler).has_value()) {
      lucent::error("irq",
                    "interrupt element 0x{:08X} has handler 0x{:08X} at [0x{:08X}], which is in NO "
                    "loaded code image; delivering it would fault. mask=0x{:08X} verifier=0x{:08X}",
                    elem,
                    handler,
                    elem + 4,
                    c->mem_r32(elem),
                    c->mem_r32(elem + 8));
    }
    lucent::debug("irq", "delivering: elem 0x{:08X} handler 0x{:08X} (I_STAT&I_MASK=0x{:03X})", elem, handler, pending);
    const auto result = psx::cpu::dispatchGuest0(*c, handler, psx::cpu::ExecutionBudget::currentTurn(*c));
    if (!result.returned()) {
      *static_cast<R3000 *>(c) = saved;
      in_irq = 0;
      psx::cpu::completeOrPropagate(*c, result);
      return;
    }
    claimed = 1;
    break;
  }
  // Declining here only says the BIOS element chain did not own this source. Games commonly route
  // CD-ROM through the custom exception exit and a separate master table, so do not misdiagnose a
  // correct VBlank-only verifier as "the game has no CD service".
  //
  // IT ALSO NO LONGER IMPLIES THAT NOTHING OWNS THE SOURCE. The framework's own CD-ROM interrupt
  // handler runs immediately below, for a title that declares the guest-interrupt delivery owner, and
  // it is the thing that serves a declined CD bit there. Read this line as "no REGISTERED ELEMENT
  // claimed it", which is what was measured, and not as "the source is unowned".
  if (!claimed) {
    static uint32_t last_unclaimed = 0xFFFFFFFFu;
    if (pending != last_unclaimed) {
      last_unclaimed = pending;
      lucent::info("irq",
                   "pending I_STAT&I_MASK=0x{:03X}; no SysEnq element claimed it "
                   "({} in chain), custom exception exit {}",
                   pending,
                   irq_n,
                   exception_exit_buf ? "installed" : "not installed");
    }
  }

  *static_cast<R3000 *>(c) = saved;

  // ---- the framework's own CD-ROM interrupt handler -------------------------------------------------
  // THE ORDER HERE IS THE DOUBLE-DELIVERY GATE, and it is the same order the hardware uses: the
  // guest's own registered elements get first refusal, and the framework's built-in per-source
  // handler runs only for a source the chain DECLINED. A title that services CD-ROM through its own
  // element therefore keeps exactly that one delivery, and a title that does not gets this one.
  //
  // It runs before the custom exception exit, and that ordering is a measured decision rather than
  // a preference: the guest's general top-level handler is the LAST resort for a declined source, and
  // this built-in handler is the BIOS's own service for the CD-ROM specifically. Nothing that
  // reached the guest through the custom exit on this title is taken away by going first, because
  // the guest reaches no CD-ROM service through it: the `cdcr` channel logged ZERO reads of
  // 0x1F801800-3 over a 4,495-presented-frame run (psxport/docs/issues/0124), with the sibling `cdc`
  // channel live at 3 lines in the same run, so that zero is "scanned, matched 0" and not "the
  // instrument never ran".
  //
  // `in_irq` is released for the duration: this is a separate handler from the chain walk above, and
  // the guest's own ready callback must be able to make BIOS calls and re-enter the CD owner without
  // the delivery re-entering itself. The callback's OWN re-entrant polls find `in_irq` set again
  // for its whole extent, which is what keeps a chain of sectors from delivering twice.
  if (cd_delivery_path && !claimed) {
    in_irq = 0; // the chain walk's handler has RETURNED; this is a separate handler
    const CdReadyDelivery delivered = deliverCdReadyCompletionOnInterrupt(*c);
    if (delivered == CdReadyDelivery::GuestExited) {
      in_irq = 0;
      return; // the guest took a typed exit out of its ready callback; stop the whole poll
    }
    *static_cast<R3000 *>(c) = saved;
    in_irq = 1; // the custom exception exit below is a guest handler in its own right
  }

  if (exception_exit_buf) {
    custom_exit_active = 1;
    const BiosInterruptDispatchResult result =
        bios_interrupt_dispatch_custom_exit(c, exception_exit_buf, dispatchCustomExceptionExit);
    custom_exit_active = 0;
    if (result != BiosInterruptDispatchResult::ReturnedFromException) {
      lucent::error("irq", "custom exception exit violated B0:0x17 contract (result={})", (int)result);
      abort();
    }
  }
  *static_cast<R3000 *>(c) = saved;
  in_irq = 0;
  const bool still_deliverable =
      (c->irqStatLatch() & i_mask) && (irq_n != 0 || exception_exit_buf != 0 || cd_delivery_path);
  if (!dma_done_any() && !still_deliverable) { // an owed DMA callback or live IRQ keeps the gate armed
    c->pending_work &= ~Core::PW_IRQ;
  }
}
