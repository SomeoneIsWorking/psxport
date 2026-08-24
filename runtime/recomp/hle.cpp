// Recomp-native HLE BIOS (S2 boot subset). Transcribed from the proven wide60 HLE
// (runtime/hle_kernel.cpp), adapted to operate directly on the Core register file and
// g_ram via the recomp memory accessors. Scope: exactly the A0/B0/C0 calls the boot path
// exercises; extended as the boot/diff harness reveals more. Faithful-first — semantics
// match the wide60 HLE that provably boots Tomba!2; not reimplemented from guesswork.
//
// State (event control blocks, native heap, IRQ / work-area flags) lives on `class Hle`
// in game.h (`c->game->hle`) — per-Core so SBS's two cores keep separate BIOS state. The
// BIOS-call dispatchers below are METHODS on that class; the `rec_syscall` / `rec_break`
// / `rec_dispatch_miss` free entries below are the C-ABI shims the recompiled shards call.
#include "bios_interrupt.h"
#include "bios_libc_string.h"
#include "cfg.h"
#include "core.h"
#include "dma_irq.h" // dma_irq_ack — the CD DMA completion dispatch below stands in for the
#include "fs_util.h" // Fs::writeFile — the miss RAM dump below
#include "game.h"
#include "guest_call.h"   // rc0 — invoke an EvMdINTR handler
#include "memcard.h"      // card_hle_a0 / card_hle_b0 — libcard BIOS dispatch (class Memcard)
#include "platform_hle.h" // class PlatformHle — sync-primitive HLE consulted on a RAM-code miss
#include "syscall_exception.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lucent/log.h>
// BIOS DMA handler, so it performs that handler's acknowledge

extern "C" void guest_backtrace_to(Core *c, FILE *out);               // sync_overrides.cpp
extern "C" void guest_find_word_to(Core *c, FILE *out, uint32_t val); // sync_overrides.cpp

// MIPS o32 register indices (== c->r[]).
enum { A0 = 4, A1 = 5, A2 = 6, A3 = 7, T1 = 9, V0 = 2 };

// ---- Event control blocks (B0:0x07-0x0D) --------------------------------------------
enum { EVCB_MAX = 16 };
static const uint32_t EV_ID_BASE = 0xF1000000u;

int Hle::eventIndex(uint32_t id) const {
  uint32_t idx = id - EV_ID_BASE;
  return (idx < EVCB_MAX && ev[idx].open) ? (int)idx : -1;
}

// Event delivery (called by the frame tick, the CD model and the memory-card model).
//
// TWO DELIVERY MODES, and only one of them used to work. An event opened EvMdNOINTR (0x2000) is POLLED:
// the game calls TestEvent, which reads and clears `fired`, so marking the slot is the whole job. An
// event opened EvMdINTR (0x1000) is a CALLBACK: the BIOS invokes the handler stored at OpenEvent, and
// the game never polls it at all. This function only ever marked slots, so every EvMdINTR event was
// registered, delivered, and silently dropped — the slot's `func` was captured and never called.
//
// That is not hypothetical: Spyro opens SwCARD (0xF4000001) spec 4 with mode 0x1000 and waits forever
// on a handler that could not run (consumer issue 0027). Marking `fired` for such a slot is worse than
// doing nothing, because it leaves state saying the event was handled when nothing ran.
//
// RE-ENTRANCY IS GUARDED. A handler may itself deliver events — the card handlers do — and without a
// depth guard a handler that (directly or otherwise) re-delivers its own class recurses until the host
// stack dies. The guard drops nested delivery rather than queueing it: the BIOS runs these at interrupt
// time with interrupts masked, so "not re-entered" is the faithful behaviour, and a silent stack
// overflow is the alternative.
//
// The register file is saved and restored around the call for the same reason vsync.cpp does it: this
// runs from INSIDE guest execution, and a real interrupt preserves the interrupted function's registers.
void Hle::deliverEvent(uint32_t evClass, uint32_t spec) {
  Core *c = &game->core;
  for (int i = 0; i < EVCB_MAX; i++) {
    if (!(ev[i].open && ev[i].enabled && ev[i].ev_class == evClass && (ev[i].spec & spec))) {
      continue;
    }
    ev[i].fired = 1;
    if (ev[i].mode != EV_MD_INTR || !ev[i].func) {
      continue;
    }
    if (ev_depth) {
      lucent::warn("ev",
                   "nested delivery of class=0x{:08X} spec=0x{:08X} dropped (handler 0x{:08X}): the BIOS "
                   "runs these with interrupts masked, so re-entry is not faithful",
                   evClass,
                   spec,
                   ev[i].func);
      continue;
    }
    ev_depth++;
    R3000 saved = *static_cast<R3000 *>(c);
    rc0(c, ev[i].func);
    *static_cast<R3000 *>(c) = saved;
    ev_depth--;
  }
}

// ---- Heap (A0:0x33-0x39): native first-fit arena, bookkeeping outside PSX RAM -------
enum { HEAP_MAX_BLOCKS = 4096 };

void Hle::heapInit(uint32_t addr, uint32_t size) {
  heap_base = addr;
  heap_size = size;
  nblk = 1;
  blk[0].addr = addr;
  blk[0].size = size;
  blk[0].used = 0;
  heap_ok = 1;
}

uint32_t Hle::heapAlloc(uint32_t size) {
  if (!heap_ok || size == 0) {
    return 0;
  }
  size = (size + 7u) & ~7u;
  for (int i = 0; i < nblk; i++) {
    if (blk[i].used || blk[i].size < size) {
      continue;
    }
    if (blk[i].size > size && nblk < HEAP_MAX_BLOCKS) {
      for (int j = nblk; j > i + 1; j--) {
        blk[j] = blk[j - 1];
      }
      blk[i + 1].addr = blk[i].addr + size;
      blk[i + 1].size = blk[i].size - size;
      blk[i + 1].used = 0;
      blk[i].size = size;
      nblk++;
    }
    blk[i].used = 1;
    return blk[i].addr;
  }
  // NO BLOCK COULD SATISFY THE REQUEST. Reported, not silently NULL: a heap whose size came from a
  // wrong InitHeap argument refuses everything, and the guest's own null check then routes around the
  // failure so the symptom surfaces arbitrarily far away. Novelty-capped (first refusal, then powers
  // of two) so it names the condition without becoming a per-call firehose.
  ++heap_refused;
  if ((heap_refused & (heap_refused - 1)) == 0) {
    lucent::warn("hle",
                 "malloc({}) REFUSED — heap base=0x{:08X} size=0x{:X} ({} block(s)); "
                 "refusal #{}. A size of 0 here means InitHeap was called with a wrong a1.",
                 size,
                 heap_base,
                 heap_size,
                 nblk,
                 heap_refused);
  }
  return 0;
}

void Hle::heapCoalesce() {
  for (int i = 0; i + 1 < nblk;) {
    if (!blk[i].used && !blk[i + 1].used) {
      blk[i].size += blk[i + 1].size;
      for (int j = i + 1; j + 1 < nblk; j++) {
        blk[j] = blk[j + 1];
      }
      nblk--;
    } else {
      i++;
    }
  }
}

void Hle::heapFree(uint32_t addr) {
  if (!addr) {
    return;
  }
  for (int i = 0; i < nblk; i++) {
    if (blk[i].addr == addr && blk[i].used) {
      blk[i].used = 0;
      heapCoalesce();
      return;
    }
  }
}

uint32_t Hle::heapBlockSize(uint32_t addr) const {
  for (int i = 0; i < nblk; i++) {
    if (blk[i].addr == addr && blk[i].used) {
      return blk[i].size;
    }
  }
  return 0;
}

// ---- Native work area for GetB0Table()/GetC0Table() ---------------------------------
// Tomba2 reads B0table[+0x16C] -> control struct; publish a self-consistent native page.
enum { HLE_B0TABLE = 0x8000F000u, HLE_C0TABLE = 0x8000F800u, HLE_WORK_BASE = 0x8000E000u };
void Hle::workAreaInit() {
  Core *c = &game->core;
  if (work_ok) {
    return;
  }
  work_ok = 1;
  c->mem_w32(HLE_B0TABLE + 0x16Cu, HLE_WORK_BASE);
  c->mem_w32(HLE_C0TABLE + 0, 0x03E00008u); // jr $ra
  c->mem_w32(HLE_C0TABLE + 4, 0);           // nop
}

// ---- BIOS DEVICE TABLE (kernel 0x150/0x154 + the DeviceControlBlocks they point at) --------------
//
// The BIOS publishes its installed devices as an array of 80-byte DeviceControlBlocks: kernel word
// 0x00000150 holds the array's ADDRESS and 0x00000154 its SIZE IN BYTES. Guest code that resolves a
// path by device prefix walks that array ITSELF rather than going through a BIOS vector. Tomba!2's
// memory-card browser does exactly that in FUN_80080940 — parse "bu" out of "bu00:*", scan the DCBs
// for a name match, then call B0:0x42 firstfile.
//
// Nothing published those two words, so the walk read base=0 and length=0x40000404 (whatever the
// guest's own use of low RAM had left there) and marched from address 0 treating every 80th word as
// a `char*` device name. The first non-zero one it reached was a BIOS-code word, strcmp dereferenced
// it, and the memory model refused the unmapped read — the user-reported "saving crashes the game".
// The crash is in the DEVICE LOOKUP, before any card I/O is attempted.
//
// DCB layout — the slots guest code is known to touch are marked:
//   +0x00 char* name      <-- FUN_80080940 strcmp target
//   +0x04 flags        +0x08 sector size   +0x0C char* description
//   +0x10 init   +0x14 open   +0x18 inout  +0x1C read   +0x20 write  +0x24 close  +0x28 ioctl
//   +0x2C exit   +0x30 action +0x34 firstfile  <-- FUN_80080940 PATCHES this  +0x38 nextfile
//   +0x3C format +0x40 chdir  +0x44 rename +0x48 remove +0x4C testdevice
//
// THE FUNCTION-POINTER SLOTS ARE ZERO AND ARE NEVER DISPATCHED, and that is a claim with a check
// behind it rather than an omission. On hardware they point into BIOS ROM; this port has no BIOS
// ROM — every device entry point is serviced by the A0/B0 HLE. The only guest that touches one is
// FUN_80080940, which saves +0x34 aside, replaces it with its own restore-trampoline FUN_80080ADC
// and calls B0:0x42; that trampoline's entire body is "put the saved pointer back, then tail-call
// it". So servicing B0:0x42 in the HLE and restoring +0x34 there (Hle::deviceUnhook, called from
// memcard.cpp's file_firstfile) reaches the same end state without dispatching a pointer that has
// no code behind it. If a game ever needs a slot to be genuinely callable, that is the moment to
// give the HLE a dispatchable stub page — not to leave a plausible-looking address in the table.
enum { HLE_DCB_TABLE = 0x8000F900u, HLE_DCB_NAMES = 0x8000FA40u, HLE_DCB_NAME_STRIDE = 16u };

uint32_t Hle::deviceFind(const char *name) const {
  Core *c = &game->core;
  for (int i = 0; i < dcb_n; i++) {
    const uint32_t dcb = HLE_DCB_TABLE + (uint32_t)i * DCB_STRIDE;
    const uint32_t np = c->mem_r32(dcb + DCB_OFF_NAME);
    if (!np) {
      continue;
    }
    int k = 0;
    for (;; k++) {
      const char got = (char)c->mem_r8(np + (uint32_t)k);
      if (got != name[k]) {
        k = -1;
        break;
      }
      if (!got) {
        break;
      }
    }
    if (k >= 0) {
      return dcb;
    }
  }
  return 0;
}

void Hle::deviceAdd(const char *name) {
  Core *c = &game->core;
  if (deviceFind(name)) {
    return; // idempotent: one DCB per device name
  }
  if (dcb_n >= (int)DCB_MAX) {
    lucent::warn("hle",
                 "device table full ({} entries) — '{}' NOT installed. Guest code resolving "
                 "that prefix will find no device and its open/firstfile will fail.",
                 (int)DCB_MAX,
                 name);
    return;
  }
  const int slot = dcb_n;
  const uint32_t dcb = HLE_DCB_TABLE + (uint32_t)slot * DCB_STRIDE;
  const uint32_t np = HLE_DCB_NAMES + (uint32_t)slot * HLE_DCB_NAME_STRIDE;
  for (uint32_t i = 0; i < DCB_STRIDE; i++) {
    c->mem_w8(dcb + i, 0);
  }
  uint32_t k = 0;
  for (; name[k] && k + 1 < HLE_DCB_NAME_STRIDE; k++) {
    c->mem_w8(np + k, (uint8_t)name[k]);
  }
  c->mem_w8(np + k, 0);
  c->mem_w32(dcb + DCB_OFF_NAME, np);
  dcb_n = slot + 1;
  // Republish both kernel words together: a base with a stale length is the exact shape that made
  // the un-published table walk hundreds of megabytes.
  c->mem_w32(KERNEL_DCB_ADDR, HLE_DCB_TABLE);
  c->mem_w32(KERNEL_DCB_SIZE, (uint32_t)dcb_n * DCB_STRIDE);
  lucent::info("hle",
               "BIOS device '{}' installed: DCB 0x{:08X} (table 0x{:08X}, {} device(s), "
               "{} bytes at kernel 0x150/0x154)",
               name,
               dcb,
               (uint32_t)HLE_DCB_TABLE,
               dcb_n,
               (uint32_t)dcb_n * DCB_STRIDE);
}

void Hle::deviceUnhook(uint32_t dcb) {
  if (!dcb) {
    return;
  }
  Core *c = &game->core;
  c->mem_w32(dcb + DCB_OFF_FIRSTFILE, 0);
  c->mem_w32(dcb + DCB_OFF_NEXTFILE, 0);
}

// LoadExec (A0:0x51) interceptor: process-scoped hook installed by native_stub at boot.
static void (*s_loadexec_hook)(Core *) = nullptr;

// BIOS threads are implemented natively (per-thread ucontext stacks) in threads.c.
uint32_t thread_open(Core *c);
uint32_t thread_close(Core *c);
void thread_change(Core *c, uint32_t handle);

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

// Deliver ONE pending interrupt to the guest's chain, exactly as the BIOS exception path would:
// walk in priority order, run each element's VERIFIER, and on the first that claims the interrupt
// run that element's HANDLER and stop.
//
// Why saving c->r[] is sufficient here, and not merely hopeful: in this substrate the generated code
// holds the ENTIRE guest register file in c->r[] — every emitted line reads and writes c->r[N]
// directly, and nothing is cached in host locals across a call. So r[1..31] + hi/lo + pc IS the
// guest CPU context, and restoring it makes the injection invisible to the interrupted function.
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
    const uint32_t cb = slot ? c->mem_r32(slot) : 0;
    lucent::debug("dmairq",
                  "owed ch{} -> callback {:08X} (slot {:08X}){}",
                  ch,
                  cb,
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
    rec_dispatch(c, cb);
    *static_cast<R3000 *>(c) = saved;
    in_irq = 0;
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
    } else if (c->override_tgt || c->coro_redirect_pc) {
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
  const uint32_t pending = c->irqStatLatch() & i_mask;
  // Clear the gate whenever there is nothing to deliver, so the common case costs one load-and-test
  // per function entry and nothing more. Re-armed by whoever raises next.
  // A DMA completion recorded DURING the callback dispatch above (the FMV player's MDEC-out handler
  // starts the NEXT strip's transfer, which can finish before it returns) leaves a channel still
  // owed. Clearing the gate then loses it, and the chain stops after one strip — measured: two
  // strips per movie decoded and then "no decode command in flight". So the gate survives while
  // anything is owed, and only the genuinely-idle case pays nothing.
  const bool has_delivery_path = irq_n != 0 || exception_exit_buf != 0;
  if ((!pending || !has_delivery_path) && !dma_done_any()) {
    c->pending_work &= ~Core::PW_IRQ;
    return;
  }
  if (!pending || !has_delivery_path) {
    return;
  }

  // These two are transient per-Core execution state consumed by the dispatch machinery. If either
  // is live we are NOT at a clean boundary, and delivering here could lose a pending redirect.
  if (c->override_tgt || c->coro_redirect_pc) {
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
      rec_dispatch(c, verifier);
      if (c->r[V0] == 0) {
        continue; // not this element's interrupt
      }
    }
    if (!handler) {
      continue;
    }
    // The BIOS passes the verifier's return to the handler; a handler that reads $a0 expects it.
    c->r[A0] = c->r[V0];
    lucent::debug("irq", "delivering: elem 0x{:08X} handler 0x{:08X} (I_STAT&I_MASK=0x{:03X})", elem, handler, pending);
    rec_dispatch(c, handler);
    claimed = 1;
    break;
  }
  // Declining here only says the BIOS element chain did not own this source. Games commonly route
  // CD-ROM through the custom exception exit and a separate master table, so do not misdiagnose a
  // correct VBlank-only verifier as "the game has no CD service".
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
  if (exception_exit_buf) {
    custom_exit_active = 1;
    const BiosInterruptDispatchResult result = bios_interrupt_dispatch_custom_exit(c, exception_exit_buf, rec_dispatch);
    custom_exit_active = 0;
    if (result != BiosInterruptDispatchResult::ReturnedFromException) {
      lucent::error("irq", "custom exception exit violated B0:0x17 contract (result={})", (int)result);
      abort();
    }
  }
  *static_cast<R3000 *>(c) = saved;
  in_irq = 0;
  const bool still_deliverable = (c->irqStatLatch() & i_mask) && (irq_n != 0 || exception_exit_buf != 0);
  if (!dma_done_any() && !still_deliverable) { // an owed DMA callback or live IRQ keeps the gate armed
    c->pending_work &= ~Core::PW_IRQ;
  }
}

// Every recompiled function wrapper calls this when Core::pending_work is non-zero (emit.py).
//
// Two INDEPENDENT kinds of deferred work share the gate word, and the order here is deliberate: the
// host turn runs FIRST. A host turn advances the frame clock and can present; a guest IRQ dispatch
// runs guest code. Doing the cheap, guest-invisible one first means a long IRQ handler cannot delay
// the frame clock by a whole dispatch.
void rec_irq_poll(Core *c) {
  // PSXPORT_DEBUG=pollregs — does taking deferred work preserve the guest's callee-saved state?
  // Both paths below save and restore the whole R3000, so the answer should be trivially yes; this
  // exists because the loop-back-edge gate demonstrably corrupts a guest global-base register on the
  // Spider-Man port, and "should be" is not evidence. Watches sp/fp/gp and the s-registers, which are
  // the ones a mid-function resume actually depends on.
  const bool w = lucent::channel_on("pollregs");
  uint32_t before[11];
  if (w) {
    before[0] = c->r[29];
    before[1] = c->r[30];
    before[2] = c->r[28];
    for (int i = 0; i < 8; i++) {
      before[3 + i] = c->r[16 + i];
    }
  }

  if (c->pending_work & Core::PW_HOST) {
    rec_host_turn(c);
  }
  if (c->pending_work & Core::PW_IRQ) {
    c->game->hle.irqPoll(c);
  }

  if (w) {
    static const char *nm[11] = {"sp", "fp", "gp", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
    uint32_t after[11] = {
        c->r[29], c->r[30], c->r[28], c->r[16], c->r[17], c->r[18], c->r[19], c->r[20], c->r[21], c->r[22], c->r[23]};
    for (int i = 0; i < 11; i++) {
      if (after[i] != before[i]) {
        lucent::error(
            "pollregs", "poll CLOBBERED {} at pc=0x{:08X}: 0x{:08X} -> 0x{:08X}", nm[i], c->pc, before[i], after[i]);
      }
    }
  }
}

// Dispatch one A0/B0/C0 BIOS call. Returns true if handled (c->r[V0] set), false otherwise.
bool Hle::dispatchBios(char table, uint32_t fn) {
  Core *c = &game->core;
  uint32_t a0 = c->r[A0], a1 = c->r[A1], a2 = c->r[A2];
  // `PSXPORT_DEBUG=bios` — EVERY BIOS call the guest makes, handled or not. The existing UNIMPL log
  // only fires for calls that fall through, which answers "what is missing" but not "what does this
  // game actually use" — and that second question is what a port needs when deciding which BIOS
  // subsystem to model. $ra is the caller: the A0/B0/C0 stubs are tail jumps (`li $t2,0xA0; jr $t2`),
  // so it still holds the real call site and links a call back to the library routine that made it.
  lucent::debug("bios",
                "{}0:0x{:02X}(0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}) from 0x{:08X}",
                table,
                fn,
                a0,
                a1,
                a2,
                c->r[A3],
                c->r[31]);
  HleEvCB *s_ev = ev; // alias so the switch bodies below read tersely
  if (table == 'A') {
    if (bios_libc_string_dispatch(c, fn)) {
      return true;
    }
    switch (fn) {
    // Sony libc rand/srand. This exact LCG also appears in linked PSX libc implementations:
    // unsigned 32-bit wrap, then the upper 15 bits. Keep the state per Hle/Game rather than using
    // host rand(), whose sequence and process-global coupling are both wrong for dual-core runs.
    case 0x2F:
      rand_state = rand_state * 0x41C64E6Du + 0x3039u;
      c->r[V0] = (rand_state >> 16) & 0x7FFFu;
      return true;
    case 0x30:
      rand_state = a0;
      c->r[V0] = 0;
      return true;
    case 0x33:
      c->r[V0] = heapAlloc(a0);
      return true; // malloc
    case 0x34:
      heapFree(a0);
      c->r[V0] = 0;
      return true; // free
    case 0x37: {
      uint32_t n = a0 * a1, p = heapAlloc(n); // calloc
      if (p) {
        for (uint32_t i = 0; i < n; i++) {
          c->mem_w8(p + i, 0);
        }
      }
      c->r[V0] = p;
      return true;
    }
    case 0x38: {
      uint32_t old = a0, ns = a1; // realloc
      if (!old) {
        c->r[V0] = heapAlloc(ns);
        return true;
      }
      if (!ns) {
        heapFree(old);
        c->r[V0] = 0;
        return true;
      }
      uint32_t np = heapAlloc(ns), os = heapBlockSize(old);
      if (np) {
        uint32_t n = os < ns ? os : ns;
        for (uint32_t i = 0; i < n; i++) {
          c->mem_w8(np + i, c->mem_r8(old + i));
        }
        heapFree(old);
      }
      c->r[V0] = np;
      return true;
    }
    // InitHeap(base, size). LOGGED AT INFO, not debug-gated, and it prints on EVERY boot because
    // both arguments come from the guest register file and a wrong `size` here is silent: the
    // native arena simply reports whatever it was given, and heapAlloc only fails once a request
    // exceeds it. A boot that initialises the heap with a leftover register value looks identical
    // to a correct one until an allocation near the limit fails, arbitrarily far away. So the
    // measured pair is stated once per boot rather than being knowable only under a debug channel.
    case 0x39:
      lucent::info("hle", "InitHeap(base=0x{:08X}, size=0x{:X} = {} bytes) from guest a0/a1", a0, a1, a1);
      heapInit(a0, a1);
      c->r[V0] = 0;
      return true; // InitHeap
    // --- BIOS libc string/memory leaves -------------------------------------------------------
    // These were absent, and absence here is SILENT DATA LOSS, not a missing feature: an
    // unhandled BIOS call logs UNIMPL, leaves $v0 holding whatever the previous call left there,
    // and — crucially — DOES NOT PERFORM THE WRITE. A guest bzero() that quietly does nothing
    // leaves a structure full of garbage that surfaces arbitrarily far away. Spider-Man's boot
    // calls A(28h) and A(2Ah) twice each; every PSX title that uses the BIOS libc calls them.
    case 0x28: // bzero(dst, n)
      for (uint32_t i = 0; i < a1; i++) {
        c->mem_w8(a0 + i, 0);
      }
      c->r[V0] = a0;
      return true;
    case 0x2A: // memcpy(dst, src, n)
      for (uint32_t i = 0; i < a2; i++) {
        c->mem_w8(a0 + i, c->mem_r8(a1 + i));
      }
      c->r[V0] = a0;
      return true;
    case 0x2B: // memset(dst, c, n)
      for (uint32_t i = 0; i < a2; i++) {
        c->mem_w8(a0 + i, (uint8_t)a1);
      }
      c->r[V0] = a0;
      return true;
    case 0x2C:       // memmove(dst, src, n) —
      if (a0 > a1) { // overlap-correct, unlike 2Ah
        for (uint32_t i = a2; i-- > 0;) {
          c->mem_w8(a0 + i, c->mem_r8(a1 + i));
        }
      } else {
        for (uint32_t i = 0; i < a2; i++) {
          c->mem_w8(a0 + i, c->mem_r8(a1 + i));
        }
      }
      c->r[V0] = a0;
      return true;
    // setjmp: fill the guest's jmp_buf with the real callee-saved set and return 0 (the
    // direct-call result). The layout is the BIOS's: ra, sp, fp, s0..s7, gp. Writing it truthfully
    // matters even though longjmp is unsupported below — a guest that INSPECTS the buffer, or
    // that longjmps and is therefore about to abort, should see the state that actually existed.
    case 0x13: { // setjmp(buf)
      static const int kSave[12] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
      for (int i = 0; i < 12; i++) {
        c->mem_w32(a0 + 4u * (uint32_t)i, c->r[kSave[i]]);
      }
      c->r[V0] = 0;
      return true;
    }
    // longjmp CANNOT be honoured here and must not be faked. Restoring the guest's $sp/$ra does
    // nothing on a static recompile: control lives on the HOST stack, and returning normally
    // would resume the setjmp caller's successor instead of unwinding to it — silently running
    // the error path's continuation with the failed operation's state still live. Stop instead;
    // a guest reaching longjmp is a real event that needs a real mechanism (host setjmp at the
    // dispatch boundary, or a coroutine unwind), not a plausible-looking return.
    case 0x14: // longjmp(buf, val)
      lucent::error("hle",
                    "A0:0x14 longjmp(buf=0x{:08X}, val={}) — psxport has no guest unwind "
                    "mechanism. Faking a return here would resume the wrong continuation with "
                    "the failed operation's state live. Refusing.",
                    a0,
                    a1);
      abort();
    // A(3Fh) printf — the GAME'S OWN diagnostics. Leaving this unimplemented threw away the most
    // direct evidence a port can get: the binary saying, in English, what it thinks went wrong.
    // This port's single most useful identification so far came from a string the executable
    // emits, and every one of these was being discarded as "UNIMPL A0:0x3F" (hundreds per boot).
    // Routed to the `guest` channel so a normal run is quiet and PSXPORT_DEBUG=guest shows it.
    case 0x3F: {
      char out[1024];
      unsigned o = 0;
      uint32_t fmt = a0, argi = 0;
      const uint32_t argreg[3] = {a1, a2, c->r[A3]};
      // Varargs past the fourth land on the guest stack, at sp+16 in the o32 layout.
      auto nextarg = [&]() -> uint32_t {
        if (argi < 3) {
          return argreg[argi++];
        }
        return c->mem_r32(c->r[29] + 16u + 4u * (argi++ - 3));
      };
      for (uint32_t i = 0; o + 1 < sizeof out; i++) {
        const char ch = (char)c->mem_r8(fmt + i);
        if (!ch) {
          break;
        }
        if (ch != '%') {
          out[o++] = ch;
          continue;
        }
        // Copy the whole conversion spec through to the host printf rather than reimplementing
        // width/precision/flags: the guest's format string is the authority on its own arguments.
        char spec[32];
        unsigned sn = 0;
        spec[sn++] = '%';
        char conv = 0;
        for (uint32_t k = i + 1; sn + 1 < sizeof spec; k++) {
          const char cc = (char)c->mem_r8(fmt + k);
          if (!cc) {
            break;
          }
          spec[sn++] = cc;
          if (strchr("diouxXcspfgeEG%", cc)) {
            conv = cc;
            i = k;
            break;
          }
        }
        spec[sn] = 0;
        if (!conv) {
          break;
        }
        char tmp[256];
        if (conv == '%') {
          out[o++] = '%';
          continue;
        }
        if (conv == 's') {
          const uint32_t p = nextarg();
          unsigned n = 0;
          while (n + 1 < sizeof tmp) {
            const char sc = (char)c->mem_r8(p + n);
            if (!sc) {
              break;
            }
            tmp[n++] = sc;
          }
          tmp[n] = 0;
          o += (unsigned)snprintf(out + o, sizeof out - o, "%s", tmp);
        } else if (conv == 'c') {
          o += (unsigned)snprintf(out + o, sizeof out - o, "%c", (char)nextarg());
        } else {
          snprintf(tmp, sizeof tmp, "%s", spec);
          o += (unsigned)snprintf(out + o, sizeof out - o, tmp, (int)nextarg());
        }
      }
      out[o < sizeof out ? o : sizeof out - 1] = 0;
      // The guest terminates its own lines; strip a trailing newline so the log stays one-per-line.
      if (o && out[o - 1] == '\n') {
        out[o - 1] = 0;
      }
      lucent::debug("guest", "{}", out);
      c->r[V0] = 0;
      return true;
    }
    case 0x44:
      c->r[V0] = 0;
      return true; // FlushCache (no-op)
    case 0x49:
      c->r[V0] = 0;
      return true; // GPU_cw (GP0 word — harmless)
    case 0x51:
      if (s_loadexec_hook) {
        s_loadexec_hook(c);
        return true;
      }
      return false; // LoadExec
    case 0x70:
      c->r[V0] = 0;
      return true; // _bu_init no-op
    case 0x71:
      c->r[V0] = 0;
      return true; // _96_init no-op
    case 0x72:
      c->r[V0] = 0;
      return true; // _96_remove no-op
    default: {
      if (card_hle_a0(fn, c)) {
        return true;
      }
      return false;
    }
    }
  }
  if (table == 'B') {
    switch (fn) {
    case 0x07:
      deliverEvent(a0, a1);
      c->r[V0] = 0;
      return true; // DeliverEvent
    case 0x08: {   // OpenEvent
      for (int i = 0; i < EVCB_MAX; i++) {
        if (!s_ev[i].open) {
          s_ev[i].open = 1;
          s_ev[i].enabled = 0;
          s_ev[i].fired = 0;
          s_ev[i].ev_class = a0;
          s_ev[i].spec = a1;
          s_ev[i].mode = a2;
          s_ev[i].func = c->r[A3];
          // `PSXPORT_DEBUG=ev` — which events a game opens, and the handle each got. A port must
          // put those CLASSES into GameConfig::irqEventClasses or the per-frame delivery has
          // nothing to deliver and every TestEvent wait spins forever. There was no way to
          // discover a game's classes short of reading its OpenEvent call sites by hand; this
          // prints them from the run. Handle is what TestEvent/WaitEvent are later called with,
          // so it also links a spinning poll back to the event it is waiting on.
          lucent::debug("ev",
                        "OpenEvent class=0x{:08X} spec=0x{:08X} mode=0x{:08X} handler=0x{:08X} -> handle=0x{:08X}",
                        a0,
                        a1,
                        a2,
                        c->r[A3],
                        EV_ID_BASE + (uint32_t)i);
          c->r[V0] = EV_ID_BASE + (uint32_t)i;
          return true;
        }
      }
      c->r[V0] = 0xFFFFFFFFu;
      return true; // table full
    }
    case 0x09: {
      int i = eventIndex(a0); // CloseEvent
      if (i >= 0) {
        s_ev[i].open = 0;
        c->r[V0] = 1;
      } else {
        c->r[V0] = 0;
      }
      return true;
    }
    case 0x0A: {
      int i = eventIndex(a0); // WaitEvent (can't block)
      if (i >= 0) {
        s_ev[i].fired = 0;
        c->r[V0] = 1;
      } else {
        c->r[V0] = 0;
      }
      return true;
    }
    case 0x0B: {
      int i = eventIndex(a0); // TestEvent (read+clear)
      if (i >= 0 && s_ev[i].fired) {
        s_ev[i].fired = 0;
        c->r[V0] = 1;
      } else {
        c->r[V0] = 0;
      }
      return true;
    }
    case 0x0C: {
      int i = eventIndex(a0); // EnableEvent
      if (i >= 0) {
        s_ev[i].enabled = 1;
        c->r[V0] = 1;
      } else {
        c->r[V0] = 0;
      }
      return true;
    }
    case 0x0D: {
      int i = eventIndex(a0); // DisableEvent
      if (i >= 0) {
        s_ev[i].enabled = 0;
        c->r[V0] = 1;
      } else {
        c->r[V0] = 0;
      }
      return true;
    }
    case 0x3F: { // B(3Fh) puts
      char out[512];
      unsigned n = 0;
      while (n + 1 < sizeof out) {
        const char ch = (char)c->mem_r8(a0 + n);
        if (!ch) {
          break;
        }
        out[n++] = ch;
      }
      out[n] = 0;
      if (n && out[n - 1] == '\n') {
        out[n - 1] = 0;
      }
      lucent::debug("guest", "{}", out);
      c->r[V0] = 0;
      return true;
    }
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16: // BIOS pad — no-op (native)
      c->r[V0] = 0;
      return true;
    case 0x17: // ReturnFromException — non-returning unwind to irqPoll's saved R3000 context
      bios_interrupt_return_from_exception(custom_exit_active != 0);
    case 0x18: // ResetEntryInt — remove the custom exception-exit context
      exception_exit_buf = 0;
      c->r[V0] = 0;
      return true;
    case 0x19: // HookEntryInt — install the guest jmp_buf restored after the BIOS chain walk
      exception_exit_buf = a0;
      bios_interrupt_trace_custom_exit(c, a0);
      c->r[V0] = 0;
      return true;
    // B0:0x35 FileWrite is NOT handled here. It used to be, unconditionally: this switch wrote fd
    // 1/2 to stderr and then returned `len` FOR EVERY fd — so a write to a memory-card file never
    // reached the card at all, and the guest was told the byte count as if it had. Two bugs in one
    // line: the save was silently discarded, and `len` is the wrong return for a card fd (see
    // memcard.cpp file_read for the libmcrd retry loop that spins on a non-zero return; that spin
    // is the user-reported SELECT MEMORY CARD softlock). memcard.cpp's `file_write` already
    // implements the console-device arm identically, so the whole call now goes to card_hle_b0
    // through the default arm below and the console path has ONE definition.
    // Pinned by tests/test_memcard_file_api.cpp, which drives THIS function rather than the card
    // module directly — precisely because an earlier case here can claim a number before the card
    // module sees it.
    case 0x0E:
      c->r[V0] = thread_open(c);
      return true; // OpenThread
    case 0x0F:
      c->r[V0] = thread_close(c);
      return true; // CloseThread
    case 0x10:
      thread_change(c, a0);
      c->r[V0] = a0;
      return true; // ChangeThread
    case 0x4A:
      c->r[V0] = 0;
      return true; // stopCard / card no-op
    case 0x4B:
      c->r[V0] = 1;
      return true; // cardInfo -> present
    case 0x56:
      workAreaInit();
      c->r[V0] = HLE_C0TABLE;
      return true; // GetC0Table
    case 0x57:
      workAreaInit();
      c->r[V0] = HLE_B0TABLE;
      return true; // GetB0Table
    case 0x5B:
      c->r[V0] = 0;
      return true; // ChangeClearPAD (no-op)
    default: {
      if (card_hle_b0(fn, c)) {
        return true;
      }
      return false;
    }
    }
  }
  if (table == 'C') {
    switch (fn) {
    // SysEnqIntRP / SysDeqIntRP. Still return the element (what callers expect), but LOG it:
    // psxport does not yet dispatch interrupts to guest code (WART-05 in the Spider-Man port), and
    // the first thing a delivery model needs is the guest InterruptElement's field layout, which
    // cannot be read off the SDK header with confidence. Dumping the four words at registration
    // time answers it from the run: whichever slots hold addresses inside the recompiled .text are
    // the handler/verifier pair.
    case 0x02:
    case 0x03:
      if (lucent::channel_on("bios")) {
        const char *which = (fn == 0x02) ? "SysEnqIntRP" : "SysDeqIntRP";
        lucent::debug("bios",
                      "{} prio={} elem=0x{:08X} -> [+0]=0x{:08X} [+4]=0x{:08X} [+8]=0x{:08X} [+C]=0x{:08X}",
                      which,
                      a0,
                      a1,
                      c->mem_r32(a1),
                      c->mem_r32(a1 + 4),
                      c->mem_r32(a1 + 8),
                      c->mem_r32(a1 + 12));
      }
      if (fn == 0x02) {
        irqEnq(a0, a1);
      } else {
        irqDeq(a1);
      }
      c->r[V0] = a1;
      return true;
    case 0x00:
    case 0x01:
    case 0x07:
    case 0x08: // kernel-table
    case 0x0A:
    case 0x0C:
    case 0x12:
    case 0x1C: // installers + RCnt
      c->r[V0] = 0;
      return true;
    default:
      return false;
    }
  }
  return false;
}

// ---- Recomp-ABI C-linkage entry points -----------------------------------------------
// The `syscall` instruction: the kernel op is selected by $a0 (not the code field). Boot uses
// Enter/ExitCriticalSection around setup. Thread ops (a0=3) need the recomp thread model.
void rec_syscall(Core *c, uint32_t code, uint32_t syscallPc) {
  (void)code;
  psx::syscall_exception::enter(c->cop0, syscallPc ? syscallPc : c->pc);
  int &irq_enabled = c->game->hle.irq_enabled;
  switch (c->r[A0]) {
  case 0:
    c->r[V0] = 0;
    break;
  // Keep COP0 Status in agreement with the flag: the guest may leave a critical section by poking
  // SR directly rather than calling ExitCriticalSection, and the two views must not diverge.
  case 1:
    c->r[V0] = irq_enabled ? 1 : 0;
    irq_enabled = 0;
    psx::syscall_exception::setReturnInterruptEnabled(c->cop0, false);
    break; // EnterCritical
  case 2:
    irq_enabled = 1;
    psx::syscall_exception::setReturnInterruptEnabled(c->cop0, true);
    c->r[V0] = 0;
    break; // ExitCritical
  default:
    lucent::info("syscall", "a0={} (unhandled kernel op)", c->r[A0]);
    c->r[V0] = 0;
  }
  psx::syscall_exception::leave(c->cop0);
}
void rec_break(Core *c, uint32_t code) {
  lucent::info("break", "code {}", code);
  (void)c;
}

void rec_dispatch_miss(Core *c, uint32_t addr) {
  // Generated entries own their exact-PC observer calls in emitted code. A target that reaches this
  // miss path has no generated body to own the boundary, so observe it exactly once here, before a
  // BIOS/HLE leaf can mutate registers. Keeping this out of the generic router prevents an indirect
  // call to generated code from being counted once by the router and again by its emitted owner.
  pc_observer_at(c, addr);
  uint32_t a = addr & 0x1FFFFFFF;
  char tbl = a == 0xA0 ? 'A' : a == 0xB0 ? 'B' : a == 0xC0 ? 'C' : 0;
  if (tbl) {
    uint32_t fn = c->r[T1] & 0xFF;
    if (c->game->hle.dispatchBios(tbl, fn)) {
      return;
    }
    // An unhandled BIOS call returns WITHOUT writing $v0, so the guest consumes a stale value as
    // this call's result. For a libc leaf that is indistinguishable from a wrong answer and it
    // corrupts silently — a strcmp that never returns 0 turned into an out-of-range table index and
    // a dereferenced constant, several frames and one subsystem away from here.
    //
    // So the libc range fails fast rather than logging and shrugging. The cutoff is the A-table's
    // libc block (0x13..0x2F): every function in it RETURNS A VALUE the caller uses. Calls outside
    // it keep the old log-and-continue, because plenty of them are genuinely ignorable and several
    // are deliberately handled as no-ops above.
    if (tbl == 'A' && fn >= 0x13 && fn <= 0x2F) {
      lucent::error("hle",
                    "\nFATAL: unimplemented BIOS A0:0x{:02X} (libc) — fail-fast.\n"
                    "  This leaf RETURNS A VALUE the guest uses, and returning without writing $v0 "
                    "hands it a stale result.\n"
                    "  That is fabricating behaviour, not a missing feature: implement it in "
                    "Hle::dispatchBios next to its siblings.\n"
                    "  caller ra=0x{:08X}  a0=0x{:08X} a1=0x{:08X} a2=0x{:08X}",
                    fn,
                    c->r[31],
                    c->r[A0],
                    c->r[A1],
                    c->r[A2]);
      fflush(stderr);
      abort();
    }
    lucent::info("hle", "UNIMPL {}0:0x{:02X}", tbl, fn);
    return;
  }
  // Non-recompiled code in RAM (loaded overlay, the boot stub, or an in-function computed-jump
  // target the recompiler routed here). Skip the low exception/scratchpad region (< 0x10000) which
  // is never a call target. First honor the PLATFORM HLE table: PSX BIOS-library HW-sync leaves
  // (libcd/libetc/libmdec) that busy-spin on an unmodelled IRQ/status bit resolve natively here.
  if (a >= 0x10000 && a < 0x200000) {
    if (auto pf = c->game->platform_hle.lookup(addr | 0x80000000u)) {
      pf(c);
      return;
    }
    // FAIL FAST: the interpreter is gone. Any RAM code that is not a recompiled MAIN function, a
    // native override, or a platform-HLE leaf is a MISS — abort with call site + backtrace.
    if (c->recMissTolerant) {
      c->recMissed = true;
      return;
    } // TEST: skip oracle-unavailable state
    extern const char *overlay_router_resident_name(Core *, uint32_t);
    const char *resov = overlay_router_resident_name(c, addr);
    if (addr >= 0x80108F9Cu && addr < 0x8018A000u) { // a MODE/area-slot overlay address
      uint32_t stage = c->mem_r32(0x801fe00cu);
      uint32_t sm = c->mem_r32(0x1f800138u);
      lucent::info("miss-state",
                   "stage=0x{:08X} sm=0x{:08X} sm[48]={} [4a]={} [4c]={} [4e]={} [50]={} [52]={} 1f80019b={} "
                   "areaidx(800bf870)={} sopsig(80109450)=0x{:08X} 1f800234={}",
                   stage,
                   sm,
                   sm ? c->mem_r16(sm + 0x48) : 0xffff,
                   sm ? c->mem_r16(sm + 0x4a) : 0xffff,
                   sm ? c->mem_r16(sm + 0x4c) : 0xffff,
                   sm ? c->mem_r16(sm + 0x4e) : 0xffff,
                   sm ? c->mem_r16(sm + 0x50) : 0xffff,
                   sm ? c->mem_r16(sm + 0x52) : 0xffff,
                   c->mem_r8(0x1f80019bu),
                   c->mem_r8(0x800bf870u),
                   c->mem_r32(0x80109450u),
                   c->mem_r8(0x1f800234u));
      // Callee-saved regs often still hold the guest caller's locals (e.g. s0 = the object node in
      // the 0x8007D208 SFX-update family) — dump them plus the node fields s0 would imply.
      lucent::info(
          "miss-regs", "s0=0x{:08X} s1=0x{:08X} s2=0x{:08X} s3=0x{:08X}", c->r[16], c->r[17], c->r[18], c->r[19]);
      uint32_t s0 = c->r[16];
      if (s0 >= 0x80000000u && s0 < 0x80200000u - 0x60u) {
        lucent::info(
            "hle",
            "[miss-node s0] +0x00=0x{:08X} +0x1c(handler)=0x{:08X} +0x0d={} +0x29={} +0x44={} +0x46=0x{:02X} +0x5c={}",
            c->mem_r32(s0),
            c->mem_r32(s0 + 0x1cu),
            c->mem_r8(s0 + 0x0du),
            c->mem_r8(s0 + 0x29u),
            (int16_t)c->mem_r16(s0 + 0x44u),
            c->mem_r8(s0 + 0x46u),
            (int16_t)c->mem_r16(s0 + 0x5cu));
      }
    }
    lucent::warn("hle",
                 "\n[recomp-MISS {}] no recompiled fn for 0x{:08X}  (caller ra=0x{:08X}, a0=0x{:08X}, "
                 "c->pc=0x{:08X})\n  resident overlay for this slot = {} (if non-A00 but addr is an A00 fn -> stale "
                 "pointer /\n  wrong overlay resident; if matches but still missed -> function-discovery gap in that "
                 "overlay)\n  not a recompiled MAIN fn / native override / platform-HLE leaf — likely overlay code or "
                 "a\n  mid-function coroutine resume. The interpreter is removed; this is fail-fast by design.",
                 c->game->hle.miss_count++,
                 addr,
                 c->r[31],
                 c->r[4],
                 c->pc,
                 resov ? resov : "(addr not in any slot range)");
    guest_backtrace_to(c, stderr);
    guest_find_word_to(c, stderr, addr | 0x80000000u);
    // DUMP RAM BEFORE DYING. A miss is exactly the moment worth analysing offline — the missing code
    // is RESIDENT right now, along with whatever overlay brought it in, and the process is about to
    // take that state to the grave. Reconstructing it afterwards from image slices means guessing
    // which overlay was loaded and where, which is the question the miss usually raises.
    // Import the dump at 0x80000000 (tools/decomp.sh) and every address matches the log verbatim.
    // PSXPORT_MISS_RAMDUMP=<path> to redirect; =0 to disable.
    {
      const char *mp = cfg_str("PSXPORT_MISS_RAMDUMP");
      if (!mp) {
        mp = "scratch/raw/miss_ram.bin";
      }
      if (strcmp(mp, "0") != 0) {
        if (Fs::writeFile(mp, c->ram, 0x200000)) {
          lucent::info("hle", "miss RAM dump -> {} (2 MB, base 0x80000000 — import with tools/decomp.sh)", mp);
        } else {
          lucent::warn("hle",
                       "miss RAM dump FAILED to write {} — analysing this miss offline will need "
                       "the state reconstructed by hand",
                       mp);
        }
      }
    }
    fflush(stderr);
    abort();
  }
  // NULL-CALLBACK NO-OP (addr == 0): a `jalr $zero`-shaped dispatch to a null function pointer. The
  // libsnd SsSeqCalled command dispatcher (FUN_80091460) reads a per-command handler slot
  // (0x80104B90..0x80104BA0) and calls it; an unhandled/meta command leaves that slot null, so the
  // guest dispatches address 0 — a no-op by intent (a call to a null callback does nothing). Both SBS
  // cores reach it identically (0-diff), i.e. it is faithful guest behavior, not a port miss. Return
  // silently: a null callback is a no-op, not a "no recompiled fn" error worth reporting.
  if ((addr & 0x1FFFFFFFu) == 0) {
    return;
  }
  lucent::info("hle",
               "[miss {}] addr 0x{:08X} (no recompiled fn / overlay)  (caller ra=0x{:08X} c->pc=0x{:08X} a0=0x{:08X})",
               c->game->hle.miss_count++,
               addr,
               c->r[31],
               c->pc,
               c->r[4]);
}
