// Core owns one complete PSX machine instance. All mutable CPU, memory, device, dispatch, and
// diagnostic state is instance-local; runtime boundaries receive Core explicitly and never select a
// process-global active machine. Public R3000 inheritance exposes the canonical register state to the
// dynarec state bridge and native service owners.
#pragma once
#include "game_iface.h" // Legacy GameConfig/GameHooks compatibility views.
#include "pc_observer.h"
#include "r3000.h"
#include "render_substrate.h" // Core owns a RenderSubstrate (host-only per-Core render substrate)
#include "spin_detector.h"
#include <stdint.h>

#ifdef __cplusplus

#include "cpu_divide.h"
#include "frame_pacer.h"
#include "guest_call_attribution.h"
#include <memory>
#include <optional>

class Game; // whole-machine owner; Core::game provides explicit access to its device owners

namespace psx::cpu {
class ExecutionControl;
class ImageCatalog;
struct ImageIdentity;
class LightrecExecutor;
class NativeDispatcher;
} // namespace psx::cpu

class Core : public R3000 {
public:
  // ---- Memory (2 MB main RAM mirrored across KUSEG/KSEG0/KSEG1; 1 KB scratchpad) ----
  uint8_t ram[0x200000];
  uint8_t scratch[0x400];

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

  uint32_t io_gpustat_toggle = 0; // GPUSTAT (0x1F801814) even/odd line bit — per-instance HW state

  // A store landing in the diagnostic watch range fires this callback with address, value, and width.
  void (*storeWatchCb)(Core *, uint32_t, uint32_t, uint32_t) = nullptr; // (core, kaddr, val, width)

  // Transient continuation used by the cooperative task owner at an executor boundary.
  uint32_t pending_guest_redirect = 0;

  // Address of the currently active native override, used to suppress recursive self-interception.
  uint32_t active_native_address = 0;

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

  // ---- SPIN DETECTOR state (runtime/psx/spin_detector.h; fatal path watchdog_spin_fault) ----
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
  std::optional<psx::cpu::ImageIdentity> currentImageIdentity(uint32_t guestAddress) const;

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
  void mem_w8(uint32_t a, uint8_t v);
  void mem_w16(uint32_t a, uint16_t v);
  void mem_w32(uint32_t a, uint32_t v);
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
  template <class Value> void writeGuestMemory(uint32_t address, Value value);

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
void gpu_gp1(uint32_t w);
void gpu_present(Core *core);
void gpu_present_ex(Core *core, int do_blit);
// M3 provenance: an owned background drawer's override records the KSEG0 packet-pool span [lo,hi) it
// produced this frame, so the OT walk classifies those prims as RQ_BACKGROUND (submit.cpp).
void gpu_bg_range_add(Core *core, uint32_t lo, uint32_t hi);
void gpu_native_load_image(Core *core, int x, int y, int w, int h, uint32_t src);

#endif // __cplusplus
