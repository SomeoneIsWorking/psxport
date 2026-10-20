// class Hle: per-Game BIOS HLE state (event blocks, heap, interrupt chain, device table) and A0/B0/C0 dispatch.
#pragma once
#include "bios_load_exec.h"
#include "execution_exit.h"
#include "guest_address_range.h"
#include "r3000.h"

#include <cstdint>
#include <optional>
class Core;
class Game;

// OpenEvent modes: EvMdINTR means 'call the handler', EvMdNOINTR means 'mark it and let TestEvent poll'.
enum : std::uint16_t { EV_MD_INTR = 0x1000, EV_MD_NOINTR = 0x2000 };

struct HleEvCB {
  int open, enabled, fired;
  uint32_t ev_class, spec, mode, func;
};
struct HleHeapBlock {
  uint32_t addr, size;
  int used;
};
class Hle {
public:
  static constexpr uint32_t kB0Table = 0x8000F000u;
  static constexpr uint32_t kWorkBase = 0x8000E000u;
  // SCPH1001 bfc108d0 loads the kernel exception stack before walking the chain; it sits under the work area.
  static constexpr uint32_t kExceptionStackTop = kWorkBase;
  // The return address an interrupt handler runs under; no code executes there.
  static constexpr uint32_t kInterruptReturnSentinel = 0xDEAD0000u;
  static constexpr int kEventSlots = 16;
  static constexpr int kHeapBlocks = 4096;
  enum class PadWorkAreaAction : std::uint8_t { Enable, Disable };

  // Guest code run as an interrupt handler: in_irq held, the register file restored on exit, and the
  // exception stack entered unless an interrupt is already running.
  class InterruptScope {
  public:
    InterruptScope(Hle &hle, Core &core);
    InterruptScope(const InterruptScope &) = delete;
    InterruptScope &operator=(const InterruptScope &) = delete;
    ~InterruptScope();
    // Restores the interrupted register file; the scope stays held.
    void restore() {
      context_.restore();
    }

  private:
    Hle &hle_;
    psx::cpu::GuestContextScope context_;
    bool nested_;
  };
  // One handler under an InterruptScope, within the current host turn.
  psx::cpu::ExecutionResult runAsInterrupt(Core &core, uint32_t entry);

  Game *game = nullptr;
  HleEvCB ev[kEventSlots] = {};
  int ev_depth = 0; // EvMdINTR delivery depth; the BIOS runs handlers with interrupts masked
  HleHeapBlock blk[kHeapBlocks] = {};
  int nblk = 0;
  uint32_t heap_base = 0;
  uint32_t heap_size = 0;
  int heap_ok = 0;
  uint32_t heap_refused = 0; // malloc requests answered NULL
  uint32_t rand_state = 1;   // A0:0x2F/0x30; Sony libc seeds 1
  int work_ok = 0;
  // B0:12/13/14 PadCardIrq lifecycle; libpad toggles reads through B0[5B]+884/+894.
  bool bios_pad_initialized = false;
  bool bios_pad_irq_started = false;
  int dcb_n = 0;                   // installed BIOS devices (see deviceAdd)
  uint32_t exception_exit_buf = 0; // B0:0x19 HookEntryInt guest jmp_buf; restored after chain walk
  int irq_enabled = 1;

  // A0:0x51 LoadAndExecute's disc, bound by Game::wireRuntimeMembers; empty refuses the load.
  psx::hle::DiscFileReader loadExecReader;

  // I_STAT / I_MASK. Only modelled sources set bits: VBlank 0, CD 2, SIO 7, SPU 9; root counters are not modelled.
  uint32_t i_stat = 0;
  uint32_t i_mask = 0;

  // SysEnqIntRP chain in priority order; element { +0 next, +4 handler, +8 verifier, +0xC pad }, re-read per delivery.
  enum : std::uint8_t { IRQ_CHAIN_MAX = 8 };
  uint32_t irq_elem[IRQ_CHAIN_MAX] = {};
  uint32_t irq_prio[IRQ_CHAIN_MAX] = {};
  int irq_n = 0;
  int in_irq = 0;             // re-entrancy guard: an ISR's own BIOS calls must not re-deliver
  int custom_exit_active = 0; // B0:0x17 may unwind only the scoped HookEntryInt dispatch

  // CD ready completions owed to this Core, delivered or declined (cd_ready_delivery.cpp).
  uint32_t cd_ready_delivered = 0;
  uint32_t cd_ready_declined = 0;

  // Delivers one pending interrupt at a call-coherent guest function boundary.
  void irqPoll(Core *c);
  static void enterExceptionStack(Core &core);
  // Full CPU-context readiness for dispatch. Pending sources and guest masks remain separate.
  bool canDispatchInterrupt(const Core &core) const;
  void irqEnq(uint32_t prio, uint32_t elem);
  void irqDeq(uint32_t elem);

  // Fires every open, enabled slot of `evClass` matching `spec`; EvMdINTR slots run their handler.
  void deliverEvent(uint32_t evClass, uint32_t spec);

  // B0:0x20: clears `fired` on the matching EvMdNOINTR slots.
  void unDeliverEvent(uint32_t evClass, uint32_t spec);

  // A0:0x33-0x39 first-fit arena; bookkeeping lives outside guest RAM.
  void heapInit(GuestSpan heap);
  uint32_t heapAlloc(uint32_t size);
  void heapFree(uint32_t addr);
  uint32_t heapBlockSize(uint32_t addr) const;
  // B0:0x56/0x57: guest-writable C0/B0 tables, C0[0x06] the ExceptionHandler entry.
  void workAreaInit();
  bool dispatchPadBios(uint32_t function);
  // One of the two work-area pad entries; whether it answers depends on guest memory (padWorkAreaAction).
  static bool isPadWorkAreaEntry(uint32_t guestAddress);
  std::optional<PadWorkAreaAction> padWorkAreaAction(uint32_t guestAddress) const;
  void applyPadWorkAreaAction(PadWorkAreaAction action);
  bool biosPadShouldService() const;

  // The kernel device table (0x150 base, 0x154 bytes) guest code walks to resolve "bu00:".
  // deviceAdd is AddDrv (B0:0x47), idempotent per name; deviceFind returns the DCB or 0.
  enum : std::uint16_t {
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
  // Undoes the guest's +0x34 restore-trampoline hook, which the native B0:0x42 never runs.
  void deviceUnhook(uint32_t dcb);
  // events: index-lookup for B0:0x08/0x09/0x0A/0x0B/0x0C/0x0D
  int eventIndex(uint32_t id) const;
  // BIOS-call dispatch (A0/B0/C0). Returns true if handled (Core V0 set).
  bool dispatchBios(char table, uint32_t fn);

private:
  void heapCoalesce();
  // A pending enabled source with a chain element, custom exit or libcd interrupt path to take it.
  bool irqDeliverable(Core &core, bool cdDeliveryPath) const;
  // Clears PW_IRQ when nothing is deliverable and no DMA completion is owed.
  void disarmIrqGateIfIdle(Core &core, bool cdDeliveryPath) const;
  void countDecline(const Core &core);

  // `irq` channel accounting of declined deliveries, and the last pending set no element claimed.
  struct IrqDeclines {
    unsigned critical = 0;
    unsigned nested = 0;
    unsigned transient = 0;
    unsigned reports = 0;
  };
  IrqDeclines irqDeclines_;
  uint32_t lastUnclaimedIrq_ = 0xFFFFFFFFu;
};
