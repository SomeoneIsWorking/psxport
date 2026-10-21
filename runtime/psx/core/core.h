// Core — one whole PSX machine instance; every boundary receives it explicitly.
#pragma once
#include "dma_irq.h"
#include "executable_write_source.h"
#include "game_runtime.h"
#include "guest_address_range.h"
#include "guest_memory_map.h"
#include "host_turn.h"
#include "irq_edge.h"
#include "r3000.h"
#include "render_substrate.h" // Core owns a RenderSubstrate (host-only per-Core render substrate)
#include "spin_detector.h"
#include "state_producer.h"
#include "store_watch.h"
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
#include <array>
#include <lucent/log.h>
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
  uint8_t ram[psx::kMainRamBytes];
  uint8_t scratch[psx::kScratchpadBytes];

  Game *game = nullptr; // back-pointer to the owning Game (set by Game's constructor)

  psx::game::GameRuntime &runtime; // the title interface

  // ---- Per-Core host-only rendering state. ----
  psx::present::RenderSubstrate rsub;
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

  psx::dma::DmaRegisters dma;
  // A restored DMA state reports a pending MDEC stall afresh.
  void forgetMdecReports() {
    mdecStallReported_ = false;
    mdecDeferNote_ = 0;
  }
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

  // Deferred work checked at guest-service points: PW_IRQ an interrupt may be deliverable, PW_HOST
  // the host field clock owes a turn.
  enum : std::uint8_t { PW_IRQ = 1, PW_HOST = 2 };
  int pending_work = 0;

  psx::cpu::HostTurnClock hostTurn;
  psx::debug::StoreWatch storeWatch;

  psx::platform::SpinDetectorState spin;

  uint32_t cop0[16] = {};

  explicit Core(psx::game::GameRuntime &runtime);
  ~Core();

  psx::cpu::ExecutionControl &executionControl();
  psx::cpu::ImageCatalog &imageCatalog();
  const psx::cpu::ImageCatalog &imageCatalog() const;
  psx::cpu::LightrecExecutor &lightrecExecutor();
  psx::cpu::NativeDispatcher &nativeDispatcher();
  psx::cpu::GuestCallCensus &guestCallCensus() {
    return guestCallCensus_;
  }
  const psx::cpu::GuestCallCensus &guestCallCensus() const {
    return guestCallCensus_;
  }
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
  // MIPS `lh` / `lb`.
  int32_t mem_r16s(uint32_t a) {
    return (int32_t)(int16_t)mem_r16(a);
  }
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
  // Without the executable-write notification: the caller notifies the whole burst afterwards.
  void mem_w32_unnotified(uint32_t a, uint32_t v);
  void mem_w8_unnotified(uint32_t a, uint8_t v);
  // A burst of bytes at `a` (a CD or module load), with one executable-write notification over it.
  void mem_write(uint32_t a, std::span<const uint8_t> bytes, psx::cpu::ExecutableWriteSource source);
  // A NUL-terminated guest string into `out`, at most cap-1 bytes, always terminated.
  void readCString(uint32_t address, char *out, size_t cap);
  // The guest bytes at `address` into `out`; one copy when they are guest storage.
  void readBytes(uint32_t address, std::span<std::byte> out);

  // Folds pending peripheral edges into I_STAT, then reads it.
  uint32_t irqStatLatch();
  // Latches `bit` in I_STAT and arms PW_IRQ.
  void raiseInterrupt(IrqBit bit);
  // I_STAT & I_MASK, after latching.
  uint32_t irqPendingEnabled();
  bool irqPendingEnabled(IrqBit bit);

  // Logs every GPR that points at a printable string in mapped RAM, for the unmapped-access report.
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
  // Every unmapped access, native or translated, passes here so the differential journal sees it.
  uint32_t deviceRead(uint32_t a, uint32_t bytes);
  void deviceWrite(uint32_t a, uint32_t v, uint32_t bytes);
  template <class Value, bool NotifyExecutable = true>
  void writeGuestMemory(uint32_t address, Value value, psx::cpu::ExecutableWriteSource source);

  // The DMA0/DMA1 ping-pong around the decoder; a start that cannot complete stays pending.
  void mdec_dma_pump();
  bool mdecStallReported_ = false;
  uint32_t mdecDeferNote_ = 0; // last traced deferral (reason<<24 | count), so a poll loop logs once
  std::array<uint32_t, 0x10000> dmaBuffer_{};
  lucent::Channel spuDmaChannel_{"spudma"};
};

#endif // __cplusplus
