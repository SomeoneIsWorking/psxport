#pragma once

#include "dynarec_capabilities.h"
#include "execution_exit.h"
#include "fallback_policy.h"
#include "function_reach.h"
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
  // THE STORE INSTRUCTION ITSELF, read from guest memory at `guestPc`. Added 2026-09-27 because its
  // ABSENCE is what stopped this instrument answering the question it is constantly used for.
  //
  // Without it a caller learns a store ran at a PC and can read the general registers, but cannot learn
  // WHERE it went: the target is `gpr[(instruction >> 21) & 0x1F] + sign_extend16(instruction)` for the
  // load/store family, and the stored value is `gpr[(instruction >> 16) & 0x1F]`. Both are one shift
  // and one add from this word, and both were previously unreachable — so a store PC could be confirmed
  // but its EFFECT could not be read, and "what wrote this guest word" stalled one step short.
  //
  // Read from the Core rather than from translated code, so it is the real guest image and stays right
  // across a recompile. Cost is one 32-bit read per observation, and observations only happen for armed
  // PCs, so an ordinary run pays nothing.
  std::uint32_t instruction = 0;
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

// Resolved destination of a load/store, from the instruction word and the register file.
//
// WHY IT IS NOT A GUESS. For the load/store family the encoding is fixed: `rs = (word >> 21) & 0x1F`,
// `rt = (word >> 16) & 0x1F`, and the displacement is the low 16 bits SIGN-EXTENDED, so the target is
// `gpr[rs] + sign_extend16(word)`. Reading the displacement unsigned puts every negative-offset store
// 0x10000 above the word it wrote, which is a plausible-looking address that is not the field — so the
// sign extension is load-bearing and `tests/test_dynarec_contract.cpp` pins all three of its edges.
struct ResolvedStoreTarget {
  std::uint32_t address = 0;
  std::uint32_t value = 0;
  std::uint32_t baseRegister = 0;
  std::uint32_t sourceRegister = 0;
  std::int32_t displacement = 0;
  bool valid = false;
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
  // The LAST resolved destination seen at this PC, and the first, because a store that always lands on
  // one address and a store that MOVES are different findings and a count alone cannot tell them apart.
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
  // Stores reported from an INSTRUMENTED BLOCK whose PC nobody asked for. Lightrec instruments per basic
  // block, so arming one PC makes every store in that block report itself, not only the armed one. This
  // counts the surplus, which is the denominator that says how much of an armed block's traffic was
  // actually requested — without it a caller cannot distinguish a quiet block from a block that was
  // never instrumented at all.
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
  std::uint64_t faults = 0;
  // Memory callbacks psxport received from translated code, device addresses and not. The
  // denominator for the two counters below: without it, "no device access committed anything" and
  // "no memory access happened" are the same zero, and the second is a broken instrument.
  std::uint64_t memoryCallbacks = 0;
  // Device accesses that committed guest time mid-segment, and the instructions they committed. A
  // commit of 0 instructions is not counted (the cycle counter had not moved), so a count of 0 with a
  // non-zero `memoryCallbacks` is a real "no commit happened", not a missing measurement.
  std::uint64_t deviceClockCommits = 0;
  std::uint64_t deviceClockCommitInstructions = 0;
  // THE PC REPORTED AT A BUDGET EXIT, classified against the loaded code images.
  //
  // BOTH exit sites count, not one. `lightrec_executor.cpp` returns BudgetExhausted from the
  // CYCLE budget and again from the HOST-DISPATCH budget, and each hands back a pc that guest
  // execution will resume at. The first version of this census instrumented only the cycle exit
  // and its own test caught the gap: a host-dispatch exit left `budgetExits` unmoved, so the
  // denominator under-reported and "0 outside every code image" would have covered only half the
  // path it claims to describe. A census with a partial feeder is the dead-tap shape again.
  //
  // A budget exit is an ordinary bounded exit: the caller commits the state, then resumes
  // deliberately at the pc the executor reports. That makes the reported pc a CONTRACT - it is
  // where guest execution will next begin - and it is the one value on that path that nothing in
  // the framework classifies. Measured 2026-09-29 on Mega Man X4: a run faulted with a fetch at
  // 0x0113D7D0 after "0 cycles", i.e. inside a resumed segment, and that address was in NO guest
  // register, in neither of the call's two `j` instruction words, and was not the logged resume
  // point. Every guest-side explanation was refuted, and the remaining candidate is this pc.
  //
  // The denominator is the point. "0 exits reported a bad pc" and "no budget exit ever happened" are
  // the same zero without `budgetExits`, and this project has been bitten by exactly that shape
  // repeatedly - a dead tap that reads as a clean measurement of absence.
  std::uint64_t budgetExits = 0;
  std::uint64_t budgetExitPcInCodeImage = 0;
  std::uint64_t budgetExitPcOutsideCodeImage = 0;
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
  // Record every guest pc passing the block boundary into `reach` (runtime/cpu/function_reach.h).
  void attachFunctionReach(std::unique_ptr<FunctionReach> reach);
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
