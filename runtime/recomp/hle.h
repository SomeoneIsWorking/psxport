// hle.h — class Hle — BIOS HLE subsystem (event control blocks, native first-fit heap, IRQ /
// work-area flags), owned by Game (`c->game->hle`, back-pointer wired in Game()). Implemented in
// hle.cpp. The deliverEvent method promotes the former free function hle_deliver_event (called by
// timing/native_boot/memcard/asset for VBlank + memcard + sound-DMA event delivery) so callers do
// c->game->hle.deliverEvent(class, spec) — no Core* arg on the surface.
#pragma once
#include <cstdint>
class Game;

// OpenEvent modes: EvMdINTR means 'call the handler', EvMdNOINTR means 'mark it and let TestEvent poll'.
enum { EV_MD_INTR = 0x1000, EV_MD_NOINTR = 0x2000 };

struct HleEvCB { int open, enabled, fired; uint32_t ev_class, spec, mode, func; };  // was EvCB
struct HleHeapBlock { uint32_t addr, size; int used; };                             // was HeapBlock
class Hle {
public:
  Game* game = nullptr;
  HleEvCB     ev[16]      = {};   // was s_ev[EVCB_MAX]
  // EvMdINTR events are delivered by CALLING their handler; this guards against a handler
  // re-entering delivery (see Hle::deliverEvent).
  int         ev_depth     = 0;
  HleHeapBlock blk[4096]  = {};   // was s_blk[HEAP_MAX_BLOCKS]
  int      nblk       = 0;        // was s_nblk
  uint32_t heap_base  = 0;        // was s_heap_base
  uint32_t heap_size  = 0;        // was s_heap_size
  int      heap_ok    = 0;        // was s_heap_ok
  int      work_ok    = 0;        // was s_work_ok
  uint32_t int_handler = 0;       // was s_int_handler (B0:0x19 HookEntryInt)
  int      irq_enabled = 1;       // was s_irq_enabled
  int      miss_count  = 0;       // recomp-miss log numbering (was s_miss)

  // I_STAT (0x1F801070) / I_MASK (0x1F801074). These had NO model at all: both reads fell through to
  // the unmapped-I/O path and returned 0, so every guest interrupt VERIFIER — which exists precisely
  // to test `I_MASK & bit` then `I_STAT & bit` — rejected unconditionally. Per-Game, like the rest of
  // the hardware state, so two Cores never share an interrupt controller.
  //
  // Only sources the framework ACTUALLY models may set a bit here. Today that is bit 2 (CDROM),
  // latched from CdcState::irq_edge. The rest stay 0 — and 0 means "this framework has no source for
  // that interrupt", NOT "the hardware did not raise it". Do not assert a bit from a free-running
  // timer to make some guest wait finish; that is fabricating an event.
  uint32_t i_stat = 0;
  uint32_t i_mask = 0;

  // ---- interrupt DELIVERY (C0:0x02/0x03 SysEnqIntRP/SysDeqIntRP + B0:0x17 ReturnFromException) ----
  // The chain of guest InterruptElement pointers the game registered, in priority order. Layout is
  // MEASURED, not taken from a header: { +0 next, +4 handler, +8 verifier, +0xC pad }. The elements
  // live in GUEST memory and only their ADDRESSES are held here, so a guest that rewrites its own
  // element between interrupts is honoured — the fields are re-read at every delivery.
  //
  // Before this existed the framework accepted every registration and discarded it, so any guest
  // waiting on an interrupt-delivered completion waited until its own timeout, forever.
  enum { IRQ_CHAIN_MAX = 8 };
  uint32_t irq_elem[IRQ_CHAIN_MAX] = {};
  uint32_t irq_prio[IRQ_CHAIN_MAX] = {};
  int      irq_n = 0;
  int      in_irq = 0;            // re-entrancy guard: an ISR's own BIOS calls must not re-deliver

  // Deliver one pending interrupt, if any, to the guest's registered chain. MUST only be called at a
  // point where guest state is call-coherent — a guest function boundary — never from inside a
  // native routine that is midway through mutating hardware state.
  void irqPoll(Core* c);
  void irqEnq(uint32_t prio, uint32_t elem);
  void irqDeq(uint32_t elem);

  // deliverEvent(evClass, spec): mark every open+enabled event slot whose class matches evClass
  //   and whose spec masks against `spec` as fired. Called by the frame VBlank tick, memcard
  //   completion, and sound-DMA completion so guest waits (TestEvent/WaitEvent) advance.
  void deliverEvent(uint32_t evClass, uint32_t spec);

  // ---- BIOS-side helpers -------------------------------------------------------
  // heap: A0:0x33-0x39 native first-fit arena (bookkeeping outside PSX RAM).
  void     heapInit(uint32_t addr, uint32_t size);
  uint32_t heapAlloc(uint32_t size);
  void     heapFree (uint32_t addr);
  uint32_t heapBlockSize(uint32_t addr) const;
  // work area (B0:0x56/0x57 GetC0Table/GetB0Table): publish a self-consistent native page.
  void     workAreaInit();
  // events: index-lookup for B0:0x08/0x09/0x0A/0x0B/0x0C/0x0D
  int      eventIndex(uint32_t id) const;
  // BIOS-call dispatch (A0/B0/C0). Returns true if handled (Core V0 set).
  bool     dispatchBios(char table, uint32_t fn);

private:
  void     heapCoalesce();   // internal free-side merge pass; only touches this instance's blk[]
};
