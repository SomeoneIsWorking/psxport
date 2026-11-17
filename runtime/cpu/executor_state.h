// executor_state.h — the guest-visible side state one executor keeps beside Core (loads in flight, the segment
// clock, counters) and the memory, COP0 and GTE accesses every guest engine runs them through, so the interpreter
// and translated blocks perform each access the same way.
#pragma once

#include "guest_executor.h"
#include "segment_clock.h"

#include <atomic>
#include <cstdint>
#include <optional>

class Core;

namespace psx::cpu {

struct NotedPlace;

// A load whose value reaches its register after the next instruction (the R3000 load delay slot).
struct DelayedLoad {
  std::uint32_t reg = 0;
  std::uint32_t value = 0;
};

// The executed-instruction window of the running segment: a block ends at a taken branch once it is spent.
struct SegmentLimits {
  std::uint64_t start = 0;
  std::uint64_t budget = 0;
};

class ExecutorState {
public:
  explicit ExecutorState(Core &owner) : core(owner) {}
  ExecutorState(const ExecutorState &) = delete;
  ExecutorState &operator=(const ExecutorState &) = delete;

  // ---- Registers ----

  // An instruction's own register write; it overrides a delayed load of the same register.
  void setGpr(std::uint32_t reg, std::uint32_t value);
  // A load's result, visible after the next instruction. A second load of the register cancels the first.
  void delayLoad(std::uint32_t reg, std::uint32_t value);
  // The register as LWL/LWR merge into it: a load still in flight to it is forwarded.
  std::uint32_t mergeBase(std::uint32_t reg) const;
  // Lands every load still in flight, so Core holds the architectural registers at an exit.
  void settleLoads();
  // MTC0/CTC0 and RFE: read-only registers ignored; a newly deliverable interrupt sets checkInterrupt.
  void writeCop0(std::uint32_t reg, std::uint32_t value);
  void returnFromException();

  // ---- GTE ----

  // A data register as MFC2/SWC2 read it; the screen FIFO is read through its projected vertex.
  std::uint32_t readGteData(std::uint32_t reg);
  // MFC2 of an SXY entry carries its place in the GPR it loads; any other transfer into a GPR ends what it carried.
  // CTC2 of TRZ hands the GTE the translation depth its GPR carries.
  void cop2Transfer(std::uint32_t instruction, std::uint32_t value);

  // ---- Memory ----

  // Commits the guest time since the last device access so the device about to be read sees it move.
  void beforeAccess(std::uint32_t address);
  // A LB/LH/LW/LBU/LHU/LWC2 or the aligned word of LWL/LWR: the raw value read at `address`, with its vertex place.
  std::uint32_t load(std::uint32_t instruction, std::uint32_t address);
  // The register value of a LB/LH/LW/LBU/LHU load from `address` (extended to 32 bits).
  std::uint32_t loadValue(std::uint32_t instruction, std::uint32_t address);
  // LWL/LWR: `base` (the register as merged into) with the bytes of the word at `address` & ~3.
  std::uint32_t loadUnaligned(std::uint32_t instruction, std::uint32_t address, std::uint32_t base);
  // A whole store instruction (SB/SH/SW/SWL/SWR/SWC2) at `pc` whose effective address is `address`, with the store
  // observer's before/after callbacks; `segmentInstructions` is the instruction count into the segment.
  void storeInstruction(std::uint32_t instruction,
                        std::uint32_t pc,
                        std::uint32_t address,
                        std::uint32_t segmentInstructions);
  // The instruction word at `pc`: main RAM (and its mirrors) or the scratchpad, never a device or the BIOS.
  bool fetch(std::uint32_t pc, std::uint32_t &instruction) const;

  Core &core;
  ExecutorCounters counters;
  SegmentClockLedger segmentClock;
  // Guest instructions executed by this executor, the clock the segment ledger commits from.
  std::uint64_t executed = 0;
  DelayedLoad pending;
  DelayedLoad arriving;
  std::atomic<bool> stopRequested{false};
  bool checkInterrupt = false;
  StoreObserverCallback storeCallback = nullptr;
  void *storeContext = nullptr;
  StoreObserverReport storeReport{};

private:
  void store(std::uint32_t instruction, std::uint32_t address, std::uint32_t value, std::uint32_t width);
  std::optional<NotedPlace> screenPlace(std::uint32_t reg, std::uint32_t value);
  void storedPlace(std::uint32_t instruction, std::uint32_t address, std::uint32_t value);
  void loadedPlace(std::uint32_t instruction, std::uint32_t address, std::uint32_t value);
  StoreObserverTargetCounts *observedStore(std::uint32_t pc);
  void observeStore(StoreObserverTargetCounts &target,
                    std::uint32_t pc,
                    std::uint32_t instruction,
                    StoreObservationPhase phase,
                    std::uint32_t segmentInstructions);
};

} // namespace psx::cpu
