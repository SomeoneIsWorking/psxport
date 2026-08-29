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

struct HleEvCB {
  int open, enabled, fired;
  uint32_t ev_class, spec, mode, func;
}; // was EvCB
struct HleHeapBlock {
  uint32_t addr, size;
  int used;
}; // was HeapBlock
class Hle {
public:
  Game *game = nullptr;
  HleEvCB ev[16] = {}; // was s_ev[EVCB_MAX]
  // EvMdINTR events are delivered by CALLING their handler; this guards against a handler
  // re-entering delivery (see Hle::deliverEvent).
  int ev_depth = 0;
  HleHeapBlock blk[4096] = {}; // was s_blk[HEAP_MAX_BLOCKS]
  int nblk = 0;                // was s_nblk
  uint32_t heap_base = 0;      // was s_heap_base
  uint32_t heap_size = 0;      // was s_heap_size
  int heap_ok = 0;             // was s_heap_ok
  // Requests heapAlloc could not satisfy. Counted rather than silently returning NULL: the BIOS
  // malloc contract IS to return NULL, so a heap initialised with the wrong size refuses every
  // request while looking — from outside — exactly like a game that never allocates. This counter is
  // the denominator behind any claim that the heap "works".
  uint32_t heap_refused = 0;
  // BIOS libc rand/srand (A0:0x2F/0x30). Per Hle, hence per Game/Core: host rand() would introduce
  // process-global cross-core state and a host-specific sequence. Sony's libc starts from seed 1.
  uint32_t rand_state = 1;
  int work_ok = 0;                 // was s_work_ok
  int dcb_n = 0;                   // installed BIOS devices (see deviceAdd)
  uint32_t exception_exit_buf = 0; // B0:0x19 HookEntryInt guest jmp_buf; restored after chain walk
  int irq_enabled = 1;             // was s_irq_enabled
  int miss_count = 0;              // recomp-miss log numbering (was s_miss)

  // I_STAT (0x1F801070) / I_MASK (0x1F801074). These had NO model at all: both reads fell through to
  // the unmapped-I/O path and returned 0, so every guest interrupt VERIFIER — which exists precisely
  // to test `I_MASK & bit` then `I_STAT & bit` — rejected unconditionally. Per-Game, like the rest of
  // the hardware state, so two Cores never share an interrupt controller.
  //
  // Only sources the framework ACTUALLY models may set a bit here: display-field completion raises
  // bit 0 (VBlank), Sio0's enabled controller /ACK request raises bit 7, CdcState::irq_edge raises bit
  // 2 (CDROM), and the vendored Beetle SPU's address-match logic raises bit 9 through spu_irq_raise
  // (hw_bind.cpp). The rest stay 0 — and 0 means "this framework has no source for that interrupt",
  // NOT "the hardware did not raise it". In particular, root-counter IRQ/status semantics remain
  // unmodelled; do not fabricate a timer event to make a guest wait finish.
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
  int irq_n = 0;
  int in_irq = 0;             // re-entrancy guard: an ISR's own BIOS calls must not re-deliver
  int custom_exit_active = 0; // B0:0x17 may unwind only the scoped HookEntryInt dispatch

  // Deliver one pending interrupt, if any, to the guest's registered chain. MUST only be called at a
  // point where guest state is call-coherent — a guest function boundary — never from inside a
  // native routine that is midway through mutating hardware state.
  void irqPoll(Core *c);
  void irqEnq(uint32_t prio, uint32_t elem);
  void irqDeq(uint32_t elem);

  // deliverEvent(evClass, spec): mark every open+enabled event slot whose class matches evClass
  //   and whose spec masks against `spec` as fired. Called by the frame VBlank tick, memcard
  //   completion, and sound-DMA completion so guest waits (TestEvent/WaitEvent) advance.
  void deliverEvent(uint32_t evClass, uint32_t spec);

  // ---- BIOS-side helpers -------------------------------------------------------
  // heap: A0:0x33-0x39 native first-fit arena (bookkeeping outside PSX RAM).
  void heapInit(uint32_t addr, uint32_t size);
  uint32_t heapAlloc(uint32_t size);
  void heapFree(uint32_t addr);
  uint32_t heapBlockSize(uint32_t addr) const;
  // work area (B0:0x56/0x57 GetC0Table/GetB0Table): publish self-consistent, guest-writable BIOS
  // tables, including the C(06h) ExceptionHandler entry address.
  void workAreaInit();

  // ---- BIOS DEVICE TABLE (kernel 0x150/0x154) --------------------------------------------------
  // The installed-device array guest code walks to resolve a path prefix ("bu00:*" -> the memory
  // card). Nothing published it before, so the walk read a garbage base/length out of low RAM and
  // dereferenced whatever it found — see hle.cpp for the full note and the DCB layout.
  //
  // deviceAdd(name) appends one DeviceControlBlock and republishes 0x150/0x154; it is the port's
  // equivalent of the BIOS AddDrv (B0:0x47) and is called by the subsystem that services the
  // device (the card registers "bu" from card_overrides_init). Idempotent per name.
  // deviceFind(name) returns that DCB's guest address, or 0 when no device matches.
  enum {
    DCB_STRIDE = 0x50u,
    DCB_MAX = 4,
    DCB_OFF_NAME = 0x00u,
    DCB_OFF_FIRSTFILE = 0x34u,
    DCB_OFF_NEXTFILE = 0x38u,
    KERNEL_DCB_ADDR = 0x00000150u,
    KERNEL_DCB_SIZE = 0x00000154u
  };
  void deviceAdd(const char *name);
  uint32_t deviceFind(const char *name) const;
  // Put a DCB's firstfile/nextfile slots back to their unhooked value. Guest code installs a
  // restore-trampoline in +0x34 before calling B0:0x42 and expects the BIOS to run it; this port
  // services B0:0x42 itself, so the HLE restores the slot instead. See memcard.cpp file_firstfile.
  void deviceUnhook(uint32_t dcb);
  // events: index-lookup for B0:0x08/0x09/0x0A/0x0B/0x0C/0x0D
  int eventIndex(uint32_t id) const;
  // BIOS-call dispatch (A0/B0/C0). Returns true if handled (Core V0 set).
  bool dispatchBios(char table, uint32_t fn);

private:
  void heapCoalesce(); // internal free-side merge pass; only touches this instance's blk[]
};
