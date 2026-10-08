// Core owns one complete PSX machine instance. All mutable CPU, memory, device, dispatch, and
// diagnostic state is instance-local; runtime boundaries receive Core explicitly and never select a
// process-global active machine. Public R3000 inheritance exposes the canonical register state to the
// dynarec state bridge and native service owners.
#pragma once
#include "dma_irq.h"
#include "executable_write_source.h"
#include "game_iface.h" // Legacy GameConfig/GameHooks compatibility views.
#include "pc_observer.h"
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
#include "gpu_device.h"
#include "guest_call_attribution.h"
#include "guest_call_census.h"
#include "ordering_table.h"
#include <memory>
#include <optional>

class Game; // whole-machine owner; Core::game provides explicit access to its device owners

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

  // ---- Framework↔game seam. GameRuntime is the owning polymorphic interface. cfg/hooks are
  // compatibility views populated only by the bounded legacy adapter; migrate their consumers into
  // GameRuntime behavior or narrow immutable fact groups rather than extending either bag. ----
  GameRuntime *runtime = nullptr;
  const GuestProgramImage *guestProgramImage = nullptr;
  const GameConfig *cfg = nullptr;
  const GameHooks *hooks = nullptr;
  void *gameCtx = nullptr;

  // ---- Per-Core host-only rendering state. Title subsystems remain behind gameCtx. ----
  RenderSubstrate rsub;
  // Optional exact-PC observer and nested-call attribution, both owned per Core.
  PcObserver pcObserver;
  psx::cpu::GuestCallAttribution callAttribution;
  // The run's resumable-guest-call tally. Per-Core like every other counter here: a process that runs
  // several machines must not fold their calls into one number.
  psx::cpu::GuestCallCensus guestCallCensus_;

  DmaRegisters dma;              // DMA controller registers + owed completions — per-instance HW state (dma_irq.h)
  psx::gpu::GpuDevice gpuDevice; // the GPU the guest reads and writes: VRAM, GPUSTAT, GPUREAD, display
  // The open producer scopes and the keys their stored packets carry into the frame record.
  psx::present::EmissionScope emission;
  // The ordering tables the title named; the walk tags each recorded primitive with its bucket.
  psx::gpu::OtTables otTables;
  // Producer object states saved this logic frame, and the renders that draw them at t.
  psx::present::FrameStates frameStates;
  psx::present::StateProducers stateProducers;

  // A store landing in the diagnostic watch range fires this callback with address, value, and width.
  void (*storeWatchCb)(Core *, uint32_t, uint32_t, uint32_t) = nullptr; // (core, kaddr, val, width)

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
  enum : int { PW_IRQ = 1, PW_HOST = 2 };
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

  Core();
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
    return host_ptr(address, bytes) != nullptr;
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
  // Copy a NUL-terminated guest string into `out` (at most cap-1 bytes, always NUL-terminated).
  // The one owner of this read; title code and runtime services call it instead of re-looping mem_r8.
  void readCString(uint32_t address, char *out, size_t cap);
  uint32_t mem_lwl(uint32_t cur, uint32_t a);
  uint32_t mem_lwr(uint32_t cur, uint32_t a);
  void mem_swl(uint32_t a, uint32_t v);
  void mem_swr(uint32_t a, uint32_t v);

  // guestMemset(dst, val, n): 0x8009A420 FUN_8009A420 — the psyq libc `memset` linked into
  //   MAIN.EXE (byte-loop over guest RAM; NOT a host memcpy since dst/n address guest space).
  //   WIDE-RE DRAFT, UNWIRED — see mem.cpp for the RE note. Recovered guest contract:
  //   dst==0 -> return 0; n<=0 -> return dst unmodified; else byte-fill and return the ORIGINAL
  //   dst (the loop's local cursor advances a copy, never the returned value).
  uint32_t guestMemset(uint32_t dst, uint8_t val, int32_t n);

  // Store watchpoints (REPL `watch` / PSXPORT_CW / PSXPORT_WWATCH).
  void mem_set_watch(uint32_t lo, uint32_t hi);
  int mem_watch_hits();
  // Programmatic write-watchpoint (SBS divergence debugger): stores landing in [lo,hi) fire this
  // Core's storeWatchCb (installed by sbs.cpp) with (this, addr, value) —
  void wwatch_arm(uint32_t lo, uint32_t hi);

  // Service pending peripheral interrupt deadlines/edges into I_STAT, then read it. PUBLIC because
  // interrupt delivery (Hle::irqPoll) has to test the same latch the guest would see.
  uint32_t irqStatLatch();

  // Fault-reporter helper: print any GPR that points at a printable C string in mapped RAM, with
  // its denominator and blind spot. A member because it needs host_ptr; public because the
  // fail-fast reporter that calls it is a free function.
  void dumpStringishRegs();

private:
  std::unique_ptr<psx::cpu::ExecutionControl> executionControl_;
  std::unique_ptr<psx::cpu::ImageCatalog> imageCatalog_;
  std::unique_ptr<psx::cpu::LightrecExecutor> lightrecExecutor_;
  std::unique_ptr<psx::cpu::NativeDispatcher> nativeDispatcher_;
  uint8_t *host_ptr(uint32_t a, uint32_t bytes);
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
  void cw_check_slow(uint32_t a, uint32_t v, int width);
  void wwatch_check_slow(uint32_t a, uint32_t v, uint32_t w);
  inline void cw_check(uint32_t a, uint32_t v, int width) {
    if (s_cw_init && !s_cw_hi) {
      return;
    }
    cw_check_slow(a, v, width);
  }
  inline void wwatch_check(uint32_t a, uint32_t v, uint32_t w) {
    if (s_ww_init && !s_ww_hi) {
      return;
    }
    wwatch_check_slow(a, v, w);
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

extern "C" {

// ---- Guest services and traps ----

// ---- COP0 (minimal) ----
uint32_t cop0_mfc(Core *c, uint32_t reg);
void cop0_mtc(Core *c, uint32_t reg, uint32_t v);

// ---- COP2 / GTE ----
uint32_t gte_read_data(uint32_t reg);
void gte_write_data(uint32_t reg, uint32_t v);
uint32_t gte_read_ctrl(uint32_t reg);
void gte_write_ctrl(uint32_t reg, uint32_t v);
void gte_op(Core *c, uint32_t insn);
// Diagnostic exact-PC variant. Both entry points run the same GTE instruction; `_at` additionally
// supplies the instruction address to an explicitly armed per-Core pre-op observer. The guest
// executor ordinarily uses Core::pc.
void gte_op_at(Core *c, uint32_t insn, uint32_t guest_pc);
inline void pc_observer_at(Core *c, uint32_t guest_pc) {
  if (c && c->pcObserver.armed()) {
    c->pcObserver.observe(c, guest_pc);
  }
}
void gte_preop_observer_arm(Core *c, GtePreOpFn fn, void *user);
void gte_op_observer_arm(Core *c, GtePreOpFn preFn, GtePostOpFn postFn, void *user);
uint64_t gte_preop_observer_disarm(Core *c); // returns armed-op denominator
uint64_t gte_preop_observer_seen(const Core *c);
// swc2 of a projected screen-XY register (DR12/13/14/15): stores to memory AND records that vertex's
// view-space Z against the written address, which is what gives the renderer native per-vertex depth.
// The executor routes only those registers here; see gte_beetle.cpp for why the pairing is exact.
void gte_store_xy(Core *c, uint32_t addr, int rt);
// Native depth, mfc2 form. gte_hold_pz snapshots the vertex's view-space Z at the `mfc2` (the last
// moment it is still that vertex's — submit loops are software pipelined); gte_record_pz consumes it
// when the GPR is stored into the packet, keyed by the address written.
void gte_hold_pz(Core *c, int gpr, int zreg);
void gte_record_pz(Core *c, uint32_t addr, int gpr);
// A word copied between buffers. gte_hold_src records where it was loaded from (captured AT the load,
// because a load may clobber its own base register); gte_copy_pz carries any recorded vertex depth
// from there to `dst`. Does nothing if the source has none — never fabricates depth. See
// gte_beetle.cpp for why both properties matter.
void gte_hold_src(Core *c, int gpr, uint32_t src);
void gte_copy_pz(Core *c, int gpr, uint32_t dst);
// Move a hold between GPRs when the guest DERIVES a value (shift/mask/add) rather than copying it —
// the packing these renderers do between projecting a vertex and writing it into a packet.
void gte_hold_move(int dst, int src);

// ---- Subsystem entry points that read/write this instance's RAM (need the Core) ----
void gpu_dma2_linked_list(Core *c, uint32_t madr);
void gpu_dma2_block(Core *c, uint32_t madr, int count, int to_gpu);

} // extern "C"

// ---- Native renderer (gpu_native.cpp) — C++ linkage; take the Core for guest-RAM reads / DMA /
// per-frame present bookkeeping (no global). gpu_gp1 is display control (no RAM) but kept here. ----
void gpu_gp0(Core *core, uint32_t w);

// Replay ONE guest packet the DrawOTag walk did not read out of guest RAM.
//
// A title's native world pass can rebuild a packet stream the guest never stored — this repository's
// Spyro terrain in-between runs the guest's own draw routine again over HOST memory and gets its
// packets back. Those packets are still guest packets: the same GP0 words, the same primitive kinds, in
// the same order the guest's ordering table would have walked them. The only thing missing is the place
// to read them from, and the only two things the submit path takes from that place are the packet's
// guest-shaped address (recorded as the item's guest packet, and read by the screen-coverage background
// classification) and each word's address (the FIFO's own source stamp). So the caller supplies both,
// as guest-shaped addresses, and everything after that — the FIFO state machine, texpage/CLUT/draw-area
// resolution, layer classification, the emit-or-queue funnel — runs exactly where it has always run.
//
// The alternative, decoding each packet into resolved quad data and calling RenderQueue::emitOrQueue
// directly, is what this exists to avoid: it would put a SECOND copy of texpage/CLUT/draw-area/blend
// resolution in a title, and the two copies would drift.
void gpu_replay_guest_packet(Core *core, uint32_t nodeAddress, const uint32_t *words, unsigned count);
void gpu_gp1(uint32_t w);
void gpu_present(Core *core);
void gpu_present_ex(Core *core, int do_blit);
// M3 provenance: an owned background drawer's override records the KSEG0 packet-pool span [lo,hi) it
// produced this frame, so the OT walk classifies those prims as RQ_BACKGROUND (submit.cpp).
void gpu_bg_range_add(Core *core, uint32_t lo, uint32_t hi);
void gpu_native_load_image(Core *core, int x, int y, int w, int h, uint32_t src);

#endif // __cplusplus
