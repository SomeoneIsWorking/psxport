#pragma once

#include "execution_exit.h"
#include "fallback_policy.h"
#include "function_reach.h"
#include "guest_address_range.h"
#include "invalidation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

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

// Views are valid only during the read-only callback. Core's cached registers are not synchronized
// until the translated segment exits; these are Lightrec's flushed architectural registers.
struct StoreObservation {
  std::uint32_t guestPc = 0;
  StoreObservationPhase phase = StoreObservationPhase::Before;
  std::uint32_t guestCycle = 0; // Relative to the current translated execution segment.
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
  Busy,
  InitializationFailed,
  InternalFailure,
};

enum class InterpreterFallbackReason : std::uint8_t {
  SelfModifyingCode,
  UnsupportedBlock,
  CompilationFailed,
  LoadDelayHazard,
  UnsafeInstructionFetch,
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
  std::uint64_t executedJitInstructions = 0;
  std::uint64_t fallbackInstructions = 0;
  std::uint64_t observations = 0;
  // Lightrec instruments whole blocks, so other stores in an armed block report too.
  std::uint64_t unrequestedObservations = 0;
};

struct InterpreterFallbackCounters {
  std::uint64_t calls = 0;
  std::uint64_t instructions = 0;
  std::uint64_t refusedCalls = 0;
  std::uint64_t compilationFailed = 0;
  std::uint64_t selfModifyingCode = 0;
  std::uint64_t unsupportedBlock = 0;
  std::uint64_t loadDelayHazard = 0;
  std::uint64_t unsafeInstructionFetch = 0;
  std::uint64_t refusedCompilationFailed = 0;
  std::uint64_t refusedSelfModifyingCode = 0;
  std::uint64_t refusedUnsupportedBlock = 0;
  std::uint64_t refusedLoadDelayHazard = 0;
  std::uint64_t refusedUnsafeInstructionFetch = 0;
};

struct ExecutorCounters {
  std::uint64_t calls = 0;
  std::uint64_t translatedBlocks = 0;
  std::uint64_t executedBlocks = 0;
  std::uint64_t executedInstructions = 0;
  std::uint64_t hostDispatches = 0;
  std::uint64_t cacheHits = 0;
  std::uint64_t cacheMisses = 0;
  std::uint64_t invalidations = 0;
  // `invalidations` split by who asked, indexed by ExecutableWriteSource.
  std::array<std::uint64_t, kExecutableWriteSourceCount> invalidationsBySource{};
  std::uint64_t faults = 0;
  // Lightrec's cost of those invalidations: calls the translated-word guard answered alone versus block walks.
  std::uint64_t lightrecInvalidationCalls = 0;
  std::uint64_t lightrecInvalidationWords = 0;
  std::uint64_t lightrecInvalidationGuards = 0;
  std::uint64_t lightrecInvalidationBlockScans = 0;
  std::uint64_t lightrecInvalidatedBlocks = 0;
  // Memory callbacks from translated code, device addresses and not.
  std::uint64_t memoryCallbacks = 0;
  // Device accesses that committed guest time mid-segment; a commit of 0 instructions is not counted.
  std::uint64_t deviceClockCommits = 0;
  std::uint64_t deviceClockCommitInstructions = 0;
  // Budget exits by whether the resume pc they hand back lies in a loaded code image.
  std::uint64_t budgetExits = 0;
  std::uint64_t budgetExitPcInCodeImage = 0;
  std::uint64_t budgetExitPcOutsideCodeImage = 0;
  InterpreterFallbackCounters fallback;
  std::int64_t maxFallbackBlocksPerExecution = 0; // the configured limit, as read for the last call
};

class LightrecExecutor {
public:
  LightrecExecutor(Core &core, FallbackPolicyProvider fallbackPolicyProvider);
  ~LightrecExecutor();
  LightrecExecutor(const LightrecExecutor &) = delete;
  LightrecExecutor &operator=(const LightrecExecutor &) = delete;

  // Service native/HLE boundaries until a requested typed exit, fault, or finite budget exhaustion.
  ExecutionResult executeUntilExit(std::uint32_t guestAddress, ExecutionBudget budget);
  ExecutionResult executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget);
  void requestStop();
  void invalidate(GuestAddressRange range, ExecutableWriteSource source = ExecutableWriteSource::Native);
  void invalidateAll(ExecutableWriteSource source = ExecutableWriteSource::Native);
  // Diagnostic only. Empty targets plus null callback/context disarm. The callback must not mutate
  // guest state, re-enter the executor, or retain snapshot views beyond the call. Context must
  // outlive the armed period; disarm before destroying it.
  StoreObserverStatus
  configureStoreObserver(std::span<const std::uint32_t> targets, StoreObserverCallback callback, void *context);
  StoreObserverReport storeObserverReport() const;
  // Record every guest pc passing the block boundary into `reach` (runtime/cpu/function_reach.h).
  void attachFunctionReach(std::unique_ptr<FunctionReach> reach);
  const ExecutorCounters &counters() const;
  bool available() const;

private:
  ExecutionResult executeWithBoundary(std::uint32_t guestAddress,
                                      std::optional<std::uint32_t> returnAddress,
                                      bool dispatchHostServices,
                                      ExecutionBudget budget);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace psx::cpu
