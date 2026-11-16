#pragma once

#include "execution_exit.h"
#include "function_reach.h"
#include "guest_address_range.h"
#include "invalidation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

class Core;

namespace psx::cpu {

inline constexpr std::size_t kMaxObservedStoreTargets = 8;

enum class StoreObservationPhase : std::uint8_t {
  Before,
  After,
};

// The destination of a load/store: gpr[rs] + sign_extend16(word); an unsigned displacement lands 0x10000 high.
struct ResolvedStoreTarget {
  std::uint32_t address = 0;
  std::uint32_t value = 0;
  std::uint32_t baseRegister = 0;
  std::uint32_t sourceRegister = 0;
  std::int32_t displacement = 0;
  bool valid = false;
};

// Views are valid only during the read-only callback: gpr is r0..r31 then LO and HI.
struct StoreObservation {
  std::uint32_t guestPc = 0;
  StoreObservationPhase phase = StoreObservationPhase::Before;
  std::uint32_t guestCycle = 0; // Relative to the current execution segment.
  std::uint32_t instruction = 0;
  ResolvedStoreTarget target{};
  std::span<const std::uint32_t, 34> gpr;
  std::span<const std::uint32_t, 32> cp0;
  std::span<const std::uint32_t, 32> cp2Data;
  std::span<const std::uint32_t, 32> cp2Control;
};

using StoreObserverCallback = void (*)(const StoreObservation &, void *) noexcept;

enum class StoreObserverStatus : std::uint8_t {
  Configured,
  InvalidConfiguration,
};

struct StoreObserverTargetCounts {
  std::uint32_t guestPc = 0;
  std::uint64_t before = 0;
  std::uint64_t after = 0;
  // First and last destinations: a store that always lands on one address differs from one that moves.
  ResolvedStoreTarget lastTarget{};
  ResolvedStoreTarget firstTarget{};
  std::uint64_t distinctTargets = 0;
  ResolvedStoreTarget previousTarget{};
};

struct StoreObserverReport {
  // Counts start at the last successful arm and remain readable after disarm.
  std::array<StoreObserverTargetCounts, kMaxObservedStoreTargets> targets{};
  std::size_t targetCount = 0;
  bool armed = false;
  std::uint64_t executedInstructions = 0;
  std::uint64_t observations = 0;
};

struct ExecutorCounters {
  std::uint64_t calls = 0;
  // Block entries: the points where host dispatch, guest return and pending work are checked.
  std::uint64_t executedBlocks = 0;
  std::uint64_t executedInstructions = 0;
  std::uint64_t hostDispatches = 0;
  std::uint64_t invalidations = 0;
  // `invalidations` split by who asked, indexed by ExecutableWriteSource.
  std::array<std::uint64_t, kExecutableWriteSourceCount> invalidationsBySource{};
  std::uint64_t faults = 0;
  // Guest loads and stores, device addresses and not.
  std::uint64_t memoryAccesses = 0;
  // Device accesses that committed guest time mid-segment; a commit of 0 instructions is not counted.
  std::uint64_t deviceClockCommits = 0;
  std::uint64_t deviceClockCommitInstructions = 0;
  // Budget exits by whether the resume pc they hand back lies in a loaded code image.
  std::uint64_t budgetExits = 0;
  std::uint64_t budgetExitPcInCodeImage = 0;
  std::uint64_t budgetExitPcOutsideCodeImage = 0;
};

// Runs guest code on an R3000 interpreter over Core's registers and memory. Blocks keep the
// recompiler's shape (one ends after an unconditional jump's delay slot, at a syscall or a
// Status/Cause write, or at a taken branch that leaves it), and each block entry is where host
// dispatch, the requested guest return and pending work are checked.
class GuestExecutor {
public:
  explicit GuestExecutor(Core &core);
  ~GuestExecutor();
  GuestExecutor(const GuestExecutor &) = delete;
  GuestExecutor &operator=(const GuestExecutor &) = delete;

  // Service native/HLE boundaries until a requested typed exit, fault, or finite budget exhaustion.
  ExecutionResult executeUntilExit(std::uint32_t guestAddress, ExecutionBudget budget);
  ExecutionResult executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget);
  // Ends the running segment at its next block entry; safe from another thread.
  void requestStop();
  // Guest code is read from memory as it runs, so a write flushes nothing; these only count.
  void invalidate(GuestAddressRange range, ExecutableWriteSource source = ExecutableWriteSource::Native);
  void invalidateAll(ExecutableWriteSource source = ExecutableWriteSource::Native);
  // Diagnostic only. Empty targets plus null callback/context disarm. The callback must not mutate
  // guest state, re-enter the executor, or retain snapshot views beyond the call. Context must
  // outlive the armed period; disarm before destroying it.
  StoreObserverStatus
  configureStoreObserver(std::span<const std::uint32_t> targets, StoreObserverCallback callback, void *context);
  StoreObserverReport storeObserverReport() const;
  // Record every guest pc passing a block entry into `reach` (runtime/cpu/function_reach.h).
  void attachFunctionReach(std::unique_ptr<FunctionReach> reach);
  const ExecutorCounters &counters() const;

private:
  ExecutionResult executeWithBoundary(std::uint32_t guestAddress,
                                      std::optional<std::uint32_t> returnAddress,
                                      bool dispatchHostServices,
                                      ExecutionBudget budget);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace psx::cpu
