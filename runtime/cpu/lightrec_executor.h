#pragma once

#include "dynarec_capabilities.h"
#include "execution_exit.h"
#include "fallback_policy.h"
#include "guest_program_image.h"

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

// Views are valid only during the read-only callback. Core's cached registers are not synchronized
// until the translated segment exits; these are Lightrec's flushed architectural registers.
struct StoreObservation {
  std::uint32_t guestPc = 0;
  StoreObservationPhase phase = StoreObservationPhase::Before;
  std::uint32_t guestCycle = 0; // Relative to the current translated execution segment.
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

struct StoreObserverTargetCounts {
  std::uint32_t guestPc = 0;
  std::uint64_t before = 0;
  std::uint64_t after = 0;
};

struct StoreObserverReport {
  // Counts start at the last successful arm and remain readable after disarm.
  std::array<StoreObserverTargetCounts, kMaxObservedStoreTargets> targets{};
  std::size_t targetCount = 0;
  bool armed = false;
  std::uint64_t executedJitInstructions = 0;
  std::uint64_t fallbackInstructions = 0;
};

enum class InterpreterFallbackReason : std::uint8_t {
  SelfModifyingCode,
  UnsupportedBlock,
  CompilationFailed,
  LoadDelayHazard,
  UnsafeInstructionFetch,
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
  std::uint64_t faults = 0;
  InterpreterFallbackCounters fallback;
};

class LightrecExecutor {
public:
  explicit LightrecExecutor(Core &core, FallbackPolicyProvider fallbackPolicyProvider = defaultFallbackPolicy);
  ~LightrecExecutor();
  LightrecExecutor(const LightrecExecutor &) = delete;
  LightrecExecutor &operator=(const LightrecExecutor &) = delete;

  ExecutionResult execute(std::uint32_t guestAddress, ExecutionBudget budget);
  // Service native/HLE boundaries until a requested typed exit, fault, or finite budget exhaustion.
  ExecutionResult executeUntilExit(std::uint32_t guestAddress, ExecutionBudget budget);
  ExecutionResult executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget);
  void requestStop();
  void invalidate(GuestAddressRange range);
  void invalidateAll();
  // Diagnostic only. Empty targets plus null callback/context disarm. The callback must not mutate
  // guest state, re-enter the executor, or retain snapshot views beyond the call. Context must
  // outlive the armed period; disarm before destroying it.
  StoreObserverStatus
  configureStoreObserver(std::span<const std::uint32_t> targets, StoreObserverCallback callback, void *context);
  StoreObserverReport storeObserverReport() const;
  const ExecutorCounters &counters() const;
  void reportFallbackTelemetry(std::string_view phase) const;
  bool available() const;
  static constexpr DynarecBackendCapabilities backendCapabilities() {
    return kLightrecBackendCapabilities;
  }

private:
  ExecutionResult executeWithBoundary(std::uint32_t guestAddress,
                                      std::optional<std::uint32_t> returnAddress,
                                      bool dispatchHostServices,
                                      ExecutionBudget budget);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace psx::cpu
