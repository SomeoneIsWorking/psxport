// Core owns one complete PSX machine instance. All mutable CPU, memory, device, dispatch, and
// diagnostic state is instance-local; runtime boundaries receive Core explicitly and never select a
// process-global active machine. Public R3000 inheritance exposes the canonical register state to the
// dynarec state bridge and native service owners.
#pragma once
#include "dma_irq.h"
#include "executable_write_source.h"
#include "game_runtime.h"
#include "guest_address_range.h"
#include "r3000.h"
#include "render_substrate.h" // Core owns a RenderSubstrate (host-only per-Core render substrate)
#include "spin_detector.h"
#include "state_producer.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus

#include "cpu_divide.h"
#include "emission_scope.h"
#include "frame_pacer.h"
#include "frame_state.h"
#include "gte.h"
#include "guest_call_attribution.h"
#include "guest_call_census.h"
#include "ordering_table.h"
#include <memory>
#include <optional>
#include <span>

class Game; // whole-machine owner; Core::game provides explicit access to its device owners

namespace psx::gpu {
class GpuDevice;
}

namespace psx::cpu {
class ExecutionControl;
class ImageCatalog;
struct ImageIdentity;
class LightrecExecutor;
class NativeDispatcher;
class SideEffectJournal;
} // namespace psx::cpu

class Core : public R3000 {
public:
  // ---- Memory (2 MB main RAM mirrored across KUSEG/KSEG0/KSEG1; 1 KB scratchpad) ----
  uint8_t ram[0x200000];
  uint8_t scratch[0x400];

  // ---- Whole-machine state (runtime/psx/state/device_cpu.cpp defines these) -------------------
  //
  // The DMA channel register shadows, the MDEC ping-pong cursors and the last SPU transfer address
  // are private because only the MMIO dispatcher (mem.cpp) writes them. A save state still has to
  // carry them: a channel left PENDING across a load is guest-visible (the guest polls CHCR bit 24),
  // and the MDEC cursor is where the next decoded word is read from. They are read and written
  // through this ONE pair rather than by making the members public, so the save-state owner cannot
  // become a second writer of them.
  struct DmaChannelShadow {
    uint32_t madr = 0, bcr = 0, chcr = 0;
  };
  struct BusShadows {
    DmaChannelShadow channel[7]; // 0,1,2,4,6 are modelled; 3,5 are read-only and stay zero
    uint32_t mdec0Addr = 0;
    int32_t mdec0Left = 0;
    uint32_t mdec1Addr = 0;
    int32_t mdec1Left = 0;
    uint32_t spuXferAddr = 0;
  };
  BusShadows busShadows() const;
  void restoreBusShadows(const BusShadows &shadows);

  Game *game = nullptr; // back-pointer to the owning Game (set by Game's constructor)

  psx::game::GameRuntime &runtime; // the title interface

  // ---- Per-Core host-only rendering state. ----
  RenderSubstrate rsub;
  // Nested-call attribution, owned per Core.
  psx::cpu::GuestCallAttribution callAttribution;
  // The run's resumable-guest-call tally. Per-Core like every other counter here: a process that runs
  // several machines must not fold their calls into one number.
  psx::cpu::GuestCallCensus guestCallCensus_;

  psx::cpu::Gte &gte() {
    return gte_;
  }
  const psx::cpu::Gte &gte() const {
    return gte_;
  }

  DmaRegisters dma; // DMA controller registers + owed completions — per-instance HW state (dma_irq.h)
  // The GPU the guest reads and writes: VRAM, GPUSTAT, GPUREAD, display.
  psx::gpu::GpuDevice &gpuDevice() {
    return *gpuDevice_;
  }
  // The open producer scopes and the keys their stored packets carry into the frame record.
  psx::present::EmissionScope emission;
  // The ordering tables the title named; the walk tags each recorded primitive with its bucket.
  psx::gpu::OtTables otTables;
  // Producer object states saved this logic frame.
  psx::present::FrameStates frameStates;

  // Transient continuation used by the cooperative task owner at an executor boundary.
  uint32_t pending_guest_redirect = 0;

  // Address of the currently active native override, used to suppress recursive self-interception.
  uint32_t active_native_address = 0;

  // The override differential's journal for the path currently executing, or null (the product
  // state). Non-owning; set and restored only by `psx::cpu::SideEffectJournal::Scope`. The device
  // funnel below, host-service dispatch, syscalls, pending work and guest-time accounting consult it.
  psx::cpu::SideEffectJournal *sideEffectJournal = nullptr;

  // Deferred work checked by the executor at bounded guest-service points.
  //
  // PW_IRQ  — set when a source raises or the guest changes I_MASK/critical state; cleared by
  //           Hle::irqPoll when it finds nothing deliverable.
  // PW_HOST — set by the host frame timer (host_turn.cpp) when real time has produced at least one
  //           new display field. This is what lets the HOST get a turn while the guest is executing
  //           straight-line code that never calls back into the runtime. It is NOT an interrupt and
  //           carries no controller state: the port owns the frame clock, and this bit only says
  //           "you are owed a turn". What that turn does is the game's registered host-turn handler.
  //
  //           Without it, a guest busy-wait paced by a per-vblank callback can never terminate,
  //           because nothing advances time between two guest instructions. Spider-Man's boot does
  //           exactly that (see spider1 docs/re-frontier.md RE-05).
  enum : std::uint8_t { PW_IRQ = 1, PW_HOST = 2 };
  int pending_work = 0;

  // ---- HOST FIELD CLOCK (runtime/psx/boot/host_turn.cpp) ------------------------------------------------
  // Per-Core. The clock is the device's, but WHICH machine owns it is that machine's business: a
  // process that runs several sessions at once (a title selector whose panels are live sessions)
  // has a guest waiting for a display field in every one of them, and a single process-global
  // registration could only pace the first. Two failures came out of that, both in a host that runs
  // more than one Core: the second session's registration was refused, so its guest was never paced
  // and outran its display field until a guest call crossed a frame boundary; and the first
  // session's teardown cleared the clock out from under the sessions still running.
  struct {
    // The registered handler — guest code the clock runs at a boundary — and the period and deadline
    // in EMULATED guest ticks. A deadline met starts a complete new period, so an update longer
    // than several periods sees one field per period rather than a burst.
    void (*fn)(Core *) = nullptr;
    uint64_t periodTicks = 0;
    uint64_t deadlineTicks = 0;
    // Re-entrancy guard: the handler runs guest code that enters guest functions, each of which
    // tests the gate, so without this a turn could nest inside itself without bound.
    bool inTurn = false;
  } hostTurn;

  // ---- SPIN DETECTOR state (runtime/psx/platform/spin_detector.h; fatal path watchdog_spin_fault) ----
  SpinDetectorState spin;

  // COP0 registers (12 = Status, 13 = Cause, 14 = EPC). Per-Core: exception state must never be
  // shared between two Cores. Status bit 0 is the master interrupt enable — see stubs.cpp.
  uint32_t cop0[16] = {};

  explicit Core(psx::game::GameRuntime &runtime);
  ~Core();

  psx::cpu::ExecutionControl &executionControl();
  psx::cpu::ImageCatalog &imageCatalog();
  psx::cpu::LightrecExecutor &lightrecExecutor();
  psx::cpu::NativeDispatcher &nativeDispatcher();
  psx::cpu::GuestCallCensus &guestCallCensus() {
    return guestCallCensus_;
  }
  const psx::cpu::GuestCallCensus &guestCallCensus() const {
    return guestCallCensus_;
  }
  std::optional<psx::cpu::ImageIdentity> currentImageIdentity(uint32_t guestAddress) const;
  std::optional<psx::cpu::ImageIdentity> currentImageIdentity(GuestAddressRange physicalRange) const;

  // Resolve a contiguous guest span to its physical main-RAM bytes using the same mapping as
  // mem_r*/mem_w*. Scratchpad, I/O, zero-length, and mirror-straddling spans have no such range.
  std::optional<GuestAddressRange> mappedMainRamRange(uint32_t address, uint32_t bytes);
  // True when the whole span is guest storage (main RAM, its mirrors, or the scratchpad), so a read
  // of it reaches no device register and cannot fault.
  bool isGuestStorage(uint32_t address, uint32_t bytes) {
    return host_ptr({address, bytes}) != nullptr;
  }

  // Memory access (delegates to host_ptr / the I/O map). PSX is little-endian == host.
  uint8_t mem_r8(uint32_t a);
  uint16_t mem_r16(uint32_t a);
  uint32_t mem_r32(uint32_t a);
  // Sign-extended halfword read (MIPS `lh`): read u16 and sign-extend to int32. Kills the pervasive
  // `(int32_t)(int16_t)c->mem_r16(a)` double-cast at every arithmetic use of a signed s16 field.
  int32_t mem_r16s(uint32_t a) {
    return (int32_t)(int16_t)mem_r16(a);
  }
  // Sign-extended byte read (MIPS `lb`): u8 → int32. Same rationale as mem_r16s for `int8_t` fields.
  int32_t mem_r8s(uint32_t a) {
    return (int32_t)(int8_t)mem_r8(a);
  }
  // The optional source labels the executable-write notification each store issues: the Lightrec
  // memory callbacks pass Cpu (a guest store), native and device paths keep the default.
  void
  mem_w8(uint32_t a, uint8_t v, psx::cpu::ExecutableWriteSource source = psx::cpu::ExecutableWriteSource::MappedStore);
  void mem_w16(uint32_t a,
               uint16_t v,
               psx::cpu::ExecutableWriteSource source = psx::cpu::ExecutableWriteSource::MappedStore);
  void mem_w32(uint32_t a,
               uint32_t v,
               psx::cpu::ExecutableWriteSource source = psx::cpu::ExecutableWriteSource::MappedStore);
  // `mem_w32` without the executable-write notification, for a burst whose caller notifies the
  // covered range once afterwards (MDEC-out DMA). Every other store check still runs. The caller
  // owns the notification: skipping it leaves stale translated code.
  void mem_w32_unnotified(uint32_t a, uint32_t v);
  void mem_w8_unnotified(uint32_t a, uint8_t v);
  // A burst of bytes at `a` (a CD or module load), with one executable-write notification over it.
  void mem_write(uint32_t a, std::span<const uint8_t> bytes, psx::cpu::ExecutableWriteSource source);
  // Copy a NUL-terminated guest string into `out` (at most cap-1 bytes, always NUL-terminated).
  // The one owner of this read; title code and runtime services call it instead of re-looping mem_r8.
  void readCString(uint32_t address, char *out, size_t cap);
  // Store watchpoints (REPL `watch` / PSXPORT_CW / PSXPORT_WWATCH).
  void mem_set_watch(GuestAddressRange range);
  int mem_watch_hits();

  // Service pending peripheral interrupt deadlines/edges into I_STAT, then read it. PUBLIC because
  // interrupt delivery (Hle::irqPoll) has to test the same latch the guest would see.
  uint32_t irqStatLatch();

  // Fault-reporter helper: print any GPR that points at a printable C string in mapped RAM, with
  // its denominator and blind spot. A member because it needs host_ptr; public because the
  // fail-fast reporter that calls it is a free function.
  void dumpStringishRegs();

private:
  psx::cpu::Gte gte_{*this};
  std::unique_ptr<psx::gpu::GpuDevice> gpuDevice_;
  std::unique_ptr<psx::cpu::ExecutionControl> executionControl_;
  std::unique_ptr<psx::cpu::ImageCatalog> imageCatalog_;
  std::unique_ptr<psx::cpu::LightrecExecutor> lightrecExecutor_;
  std::unique_ptr<psx::cpu::NativeDispatcher> nativeDispatcher_;
  uint8_t *host_ptr(GuestSpan span);
  uint32_t io_read(uint32_t a, uint32_t bytes);
  void io_write(uint32_t a, uint32_t v, uint32_t bytes);
  // THE DEVICE FUNNEL: every guest-memory access with no RAM mapping, from native code and from
  // translated Lightrec code alike, passes here on its way to io_read/io_write, so the override
  // differential's journal sees (and on its shadow path, replays) each one in order.
  uint32_t deviceRead(uint32_t a, uint32_t bytes);
  void deviceWrite(uint32_t a, uint32_t v, uint32_t bytes);
  template <class Value, bool NotifyExecutable = true>
  void writeGuestMemory(uint32_t address, Value value, psx::cpu::ExecutableWriteSource source);

  // WATCH HOOKS — every guest store calls these, so their DISABLED path is on the hottest path in
  // the runtime. Profiling put cw_check at 3.1-3.7% and wwatch_check at 1.8% of total CPU with no
  // watch armed at all: almost none of that is the range test, it is the out-of-line CALL itself,
  // made once per store to reach a function that immediately returns.
  //
  // So the "is anything armed" test lives HERE, inline, and only the armed case takes a call. Before
  // the first store the armed flag is unknown, so the slow path runs once to read the environment and
  // set it — which is why the test is `initialised && !armed` rather than just `!armed`.
  void cw_check_slow(GuestStore store);
  void wwatch_check_slow(GuestStore store);
  inline void cw_check(GuestStore store) {
    if (s_cw_init && !s_cw_hi) {
      return;
    }
    cw_check_slow(store);
  }
  inline void wwatch_check(GuestStore store) {
    if (s_ww_init && !s_ww_hi) {
      return;
    }
    wwatch_check_slow(store);
  }

  // DMA channel state (per-instance) — DMA0 MDEC-in, 1 MDEC-out, 2 GPU, 4 SPU, 6 OTC.
  uint32_t s_dma0_madr = 0, s_dma0_bcr = 0, s_dma0_chcr = 0;
  uint32_t s_dma1_madr = 0, s_dma1_bcr = 0, s_dma1_chcr = 0;

  // MDEC DMA pending-channel state. On real hardware DMA0 (MDEC-in) and DMA1 (MDEC-out) sit
  // PENDING with CHCR bit 24 set and ping-pong around the decoder, each gated per block by the
  // decoder's readiness (vendor beetle-psx dma.c: ChCan -> MDEC_DMACanWrite/CanRead). This model's
  // transfers are synchronous, so a start that cannot complete latches its remainder here — busy
  // stays SET, exactly as hardware shows — and mdec_dma_pump() moves it forward on the counterpart
  // channel's start and on every guest-visible poll (DMA0/DMA1 CHCR reads, MDEC status/data reads).
  // The per-instance Beetle MDEC this pumps is bound per frame-step (MdecDevice::bind).
  uint32_t s_mdec0_addr = 0;
  int s_mdec0_left = 0; // DMA0: next guest word to read, words still to feed
  uint32_t s_mdec1_addr = 0;
  int s_mdec1_left = 0;           // DMA1: running CurAddr (dma.c form), words still to drain
  int s_mdec_stall_reported = 0;  // one loud wedge report per latched start, not per poll
  uint32_t s_mdec_defer_note = 0; // last traced deferral (reason<<24|count): trace state
                                  // CHANGES, not every poll — a pending remainder the
                                  // guest never cleans up is polled millions of times
  void mdec_dma_pump();
  uint32_t s_dma2_madr = 0, s_dma2_bcr = 0, s_dma2_chcr = 0;
  uint32_t s_dma4_madr = 0, s_dma4_bcr = 0, s_dma4_chcr = 0;
  uint32_t s_spu_xfer_addr = 0; // last SPU transfer-start addr (reg 0x1F801DA6 << 3), for SPU-DMA logging
  uint32_t s_dma6_madr = 0, s_dma6_bcr = 0, s_dma6_chcr = 0;
  uint32_t s_dma_buf[0x10000];

  // Watchpoint state.
  int s_cw_init = 0, s_cw_n = 0; // read by the inline cw_check above
  uint32_t s_cw_lo = 0, s_cw_hi = 0;
  int s_ww_init = 0;
  uint32_t s_ww_lo = 0, s_ww_hi = 0;
};

// Native services receive their Core explicitly and operate on that instance's register and memory
// state.
typedef void (*OverrideFn)(Core *);

#endif // __cplusplus
