#include "lightrec_executor.h"

#include "core.h"
#include "execution_control.h"
#include "execution_services.h"
#include "game.h"
#include "gte_register_transfer.h"
#include "hw_bind.h"
#include "native_dispatch.h"

#include <lightrec.h>
#include <lucent/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace psx::cpu {
namespace {

constexpr std::uint32_t kRamSize = 0x200000u;
constexpr std::uint32_t kBiosBase = 0x1fc00000u;
constexpr std::uint32_t kBiosSize = 0x80000u;
constexpr std::uint32_t kScratchBase = 0x1f800000u;
constexpr std::uint32_t kScratchSize = 0x400u;
constexpr std::uint32_t kHardwareBase = 0x1f801000u;
constexpr std::uint32_t kHardwareSize = 0x2000u;
static_assert(kMaxObservedStoreTargets == LIGHTREC_STORE_OBSERVER_TARGETS);

std::uint32_t targetCycle(ExecutionBudget budget) {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(budget.cycles, std::numeric_limits<std::uint32_t>::max()));
}

std::uint64_t nextBoundaryOwnerId() {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

struct LightrecExecutor::Impl {
  explicit Impl(Core &owner, FallbackPolicyProvider provider) : core(owner), fallbackPolicyProvider(provider) {
    operations = {
        .cop2_notify = nullptr,
        .cop2_op = cop2Operation,
        .enable_ram = enableRam,
        .hw_direct = nullptr,
        .code_inv = nullptr,
        .block_boundary = blockBoundary,
        .block_boundary_data = this,
        .fallback_admission = fallbackAdmission,
        .fallback_admission_data = this,
    };
    initializeMaps();
  }

  bool ensureInitialized() {
    std::scoped_lock lifecycleLock(lifecycleMutex());
    if (state || initializationAttempted) {
      return state != nullptr;
    }
    initializationAttempted = true;
    {
      std::scoped_lock lock(registryMutex());
      if (!registry().empty()) {
        lucent::error("executor", "Lightrec supports one initialized machine per process");
        return false;
      }
    }
    char programName[] = "psxport";
    state = lightrec_init(programName, maps.data(), maps.size(), &operations);
    if (!state) {
      lucent::error("executor", "Lightrec initialization failed");
      return false;
    }
    std::scoped_lock lock(registryMutex());
    registry().emplace(state, this);
    return true;
  }

  ~Impl() {
    if (!state) {
      return;
    }
    std::scoped_lock lifecycleLock(lifecycleMutex());
    {
      std::scoped_lock lock(registryMutex());
      registry().erase(state);
    }
    lightrec_destroy(state);
  }

  static Impl &owner(lightrec_state *lightrec) {
    std::scoped_lock lock(registryMutex());
    const auto found = registry().find(lightrec);
    if (found == registry().end()) {
      lucent::error("executor", "Lightrec callback has no owning Core");
      std::abort();
    }
    return *found->second;
  }

  static void storeByte(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w8(address, static_cast<std::uint8_t>(value));
  }

  static void storeHalf(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w16(address, static_cast<std::uint16_t>(value));
  }

  static void storeWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w32(address, value);
  }

  static void
  storeUnalignedWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w32(address, value);
  }

  static std::uint8_t loadByte(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address) {
    return owner(lightrec).core.mem_r8(address);
  }

  static std::uint16_t loadHalf(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address) {
    return owner(lightrec).core.mem_r16(address);
  }

  static std::uint32_t loadWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address) {
    return owner(lightrec).core.mem_r32(address);
  }

  static void cop2Operation(lightrec_state *lightrec, std::uint32_t opcode) {
    Impl &impl = owner(lightrec);
    lightrec_registers *registers = lightrec_get_registers(lightrec);
    gte_bind(&impl.core);
    gte_import_registers(impl.core.game->gte, registers->cp2d, registers->cp2c);
    gte_op_at(&impl.core, opcode, impl.core.pc);
    gte_export_registers(impl.core.game->gte, registers->cp2d, registers->cp2c);
  }

  static void enableRam(lightrec_state *, bool) {
    // Core owns one coherent RAM image and does not expose a separate cache-isolation byte array.
  }

  static const lightrec_mem_map_ops &memoryOps() {
    static const lightrec_mem_map_ops operations{
        .sb = storeByte,
        .sh = storeHalf,
        .sw = storeWord,
        .lb = loadByte,
        .lh = loadHalf,
        .lw = loadWord,
        .lwu = loadWord,
        .swu = storeUnalignedWord,
    };
    return operations;
  }

  enum class BoundaryReason : std::uint8_t {
    None,
    GuestReturn,
    HostDispatch,
    PendingWork,
  };

  struct BoundaryContext {
    std::optional<std::uint32_t> returnAddress;
    bool dispatchHostServices = false;
    bool skipPendingBoundaryOnce = false;
    BoundaryReason reason = BoundaryReason::None;
    std::uint32_t pc = 0;
    FallbackPolicy fallbackPolicy{};
    std::uint64_t admittedFallbackBlocks = 0;
    std::uint32_t fallbackRefusalPc = 0;
    lightrec_fallback_reason fallbackRefusalReason = LIGHTREC_FALLBACK_NONE;
    bool active = false;
  };

  static std::unordered_map<std::uint64_t, BoundaryContext> &threadBoundaries() {
    static thread_local std::unordered_map<std::uint64_t, BoundaryContext> boundaries;
    return boundaries;
  }

  BoundaryContext &activeBoundary() {
    auto &boundaries = threadBoundaries();
    const auto found = boundaries.find(boundaryOwnerId);
    if (found == boundaries.end() || !found->second.active) {
      lucent::error("executor", "Lightrec callback reached without a boundary on its host thread");
      std::abort();
    }
    return found->second;
  }

  static lightrec_fallback_action
  fallbackAdmission(lightrec_state *, const lightrec_fallback_event *event, void *userData) {
    auto &impl = *static_cast<Impl *>(userData);
    auto &boundary = impl.activeBoundary();
    if (boundary.admittedFallbackBlocks < boundary.fallbackPolicy.maxBlocksPerExecution) {
      ++boundary.admittedFallbackBlocks;
      return LIGHTREC_FALLBACK_ALLOW;
    }
    boundary.fallbackRefusalPc = event->guest_pc;
    boundary.fallbackRefusalReason = event->reason;
    return LIGHTREC_FALLBACK_REFUSE;
  }

  static void observeStore(const lightrec_registers *registers,
                           std::uint32_t guestPc,
                           lightrec_store_observer_phase phase,
                           std::uint32_t cycle,
                           void *userData) noexcept {
    auto &impl = *static_cast<Impl *>(userData);
    for (std::size_t i = 0; i < impl.storeReport.targetCount; ++i) {
      auto &target = impl.storeReport.targets[i];
      if (target.guestPc != guestPc) {
        continue;
      }
      if (phase == LIGHTREC_STORE_BEFORE) {
        ++target.before;
      } else {
        ++target.after;
      }
      const StoreObservation observation{
          guestPc,
          phase == LIGHTREC_STORE_BEFORE ? StoreObservationPhase::Before : StoreObservationPhase::After,
          cycle,
          std::span(registers->gpr),
          std::span(registers->cp0),
          std::span(registers->cp2d),
          std::span(registers->cp2c),
      };
      impl.storeCallback(observation, impl.storeContext);
      return;
    }
    std::abort(); // A translated callback for an unregistered PC violates the per-state target contract.
  }

  static lightrec_block_boundary_action
  blockBoundary(lightrec_state *, std::uint32_t guestPc, std::uint32_t *, void *userData) {
    auto &impl = *static_cast<Impl *>(userData);
    auto &boundary = impl.activeBoundary();
    if (boundary.returnAddress && guestPc == *boundary.returnAddress) {
      boundary.reason = BoundaryReason::GuestReturn;
    } else if (boundary.skipPendingBoundaryOnce) {
      boundary.skipPendingBoundaryOnce = false;
    } else if (impl.core.game && impl.core.active_native_address == 0 && impl.core.pending_guest_redirect == 0 &&
               __atomic_load_n(&impl.core.pending_work, __ATOMIC_RELAXED) != 0) {
      boundary.reason = BoundaryReason::PendingWork;
    }
    if (boundary.reason == BoundaryReason::None && boundary.dispatchHostServices &&
        classifyGuestHostDispatch(impl.core, guestPc) != GuestHostDispatchKind::ExecuteGuest) {
      boundary.reason = BoundaryReason::HostDispatch;
    }
    if (boundary.reason == BoundaryReason::None) {
      return LIGHTREC_BLOCK_CONTINUE;
    }
    boundary.pc = guestPc;
    return LIGHTREC_BLOCK_STOP;
  }

  class BoundarySession {
  public:
    BoundarySession(Impl &impl,
                    std::optional<std::uint32_t> returnAddress,
                    bool dispatchHostServices,
                    FallbackPolicy fallbackPolicy)
        : impl_(impl), boundary_(threadBoundaries()[impl.boundaryOwnerId]), previous_(boundary_) {
      boundary_ = {.returnAddress = returnAddress,
                   .dispatchHostServices = dispatchHostServices,
                   .fallbackPolicy = fallbackPolicy,
                   .active = true};
    }

    ~BoundarySession() {
      if (previous_.active) {
        boundary_ = previous_;
      } else {
        threadBoundaries().erase(impl_.boundaryOwnerId);
      }
    }

  private:
    Impl &impl_;
    BoundaryContext &boundary_;
    BoundaryContext previous_;
  };

  static std::unordered_map<lightrec_state *, Impl *> &registry() {
    static std::unordered_map<lightrec_state *, Impl *> instances;
    return instances;
  }

  static std::mutex &registryMutex() {
    static std::mutex mutex;
    return mutex;
  }

  static std::mutex &lifecycleMutex() {
    static std::mutex mutex;
    return mutex;
  }

  void initializeMaps() {
    maps.fill({});
    maps[PSX_MAP_KERNEL_USER_RAM] = {
        .pc = 0,
        .length = kRamSize,
        .address = core.ram,
        .ops = &memoryOps(),
    };
    maps[PSX_MAP_BIOS] = {
        .pc = kBiosBase,
        .length = kBiosSize,
        .address = bios.data(),
    };
    maps[PSX_MAP_SCRATCH_PAD] = {
        .pc = kScratchBase,
        .length = kScratchSize,
        .address = core.scratch,
        .ops = &memoryOps(),
    };
    maps[PSX_MAP_HW_REGISTERS] = {
        .pc = kHardwareBase,
        .length = kHardwareSize,
        .address = core.ram,
        .ops = &memoryOps(),
    };
    maps[PSX_MAP_MIRROR1] = {
        .pc = 0x00200000u,
        .length = kRamSize,
        .mirror_of = &maps[PSX_MAP_KERNEL_USER_RAM],
    };
    maps[PSX_MAP_MIRROR2] = {
        .pc = 0x00400000u,
        .length = kRamSize,
        .mirror_of = &maps[PSX_MAP_KERNEL_USER_RAM],
    };
    maps[PSX_MAP_MIRROR3] = {
        .pc = 0x00600000u,
        .length = kRamSize,
        .mirror_of = &maps[PSX_MAP_KERNEL_USER_RAM],
    };
  }

  void copyCoreToLightrec() {
    lightrec_registers *registers = lightrec_get_registers(state);
    std::copy_n(core.r, 32, registers->gpr);
    registers->gpr[32] = core.lo;
    registers->gpr[33] = core.hi;
    std::copy_n(core.cop0, 16, registers->cp0);
    gte_bind(&core);
    gte_export_registers(core.game->gte, registers->cp2d, registers->cp2c);
  }

  void copyLightrecToCore(std::uint32_t nextPc) {
    lightrec_registers *registers = lightrec_get_registers(state);
    std::copy_n(registers->gpr, 32, core.r);
    core.r[0] = 0;
    core.lo = registers->gpr[32];
    core.hi = registers->gpr[33];
    core.pc = nextPc;
    std::copy_n(registers->cp0, 16, core.cop0);
    gte_bind(&core);
    gte_import_registers(core.game->gte, registers->cp2d, registers->cp2c);
  }

  void updateCounters(const lightrec_execution_stats &stats) {
    counters.translatedBlocks = stats.translated_blocks;
    counters.executedBlocks = stats.executed_blocks;
    counters.executedInstructions = stats.executed_instructions + stats.fallback_instructions;
    counters.cacheHits = stats.cache_hits;
    counters.cacheMisses = stats.cache_misses;
    counters.fallback.calls = stats.fallback_blocks;
    counters.fallback.instructions = stats.fallback_instructions;
    counters.fallback.refusedCalls = stats.refused_fallback_blocks;
    counters.fallback.selfModifyingCode = stats.fallback_blocks_by_reason[LIGHTREC_FALLBACK_SELF_MODIFYING_CODE];
    counters.fallback.unsupportedBlock = stats.fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW];
    counters.fallback.compilationFailed = stats.fallback_blocks_by_reason[LIGHTREC_FALLBACK_JIT_COMPILE_FAILURE];
    counters.fallback.loadDelayHazard = stats.fallback_blocks_by_reason[LIGHTREC_FALLBACK_LOAD_DELAY_HAZARD];
    counters.fallback.unsafeInstructionFetch = stats.fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSAFE_FETCH];
    counters.fallback.refusedSelfModifyingCode =
        stats.refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_SELF_MODIFYING_CODE];
    counters.fallback.refusedUnsupportedBlock =
        stats.refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW];
    counters.fallback.refusedCompilationFailed =
        stats.refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_JIT_COMPILE_FAILURE];
    counters.fallback.refusedLoadDelayHazard =
        stats.refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_LOAD_DELAY_HAZARD];
    counters.fallback.refusedUnsafeInstructionFetch =
        stats.refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSAFE_FETCH];
  }

  void reportFallbackTelemetry(std::string_view phase, lucent::Level level) const {
    lucent::log(level,
                "executor",
                lucent::format("Lightrec fallback telemetry [{}]: executor_calls={} executed_blocks={} "
                               "executed_instructions={} fallback_blocks={} fallback_instructions={} "
                               "reasons{{compilation_failed={},self_modifying_code={},unsupported_block={},"
                               "load_delay_hazard={},unsafe_instruction_fetch={}}} refused_fallback_blocks={} "
                               "refused_reasons{{compilation_failed={},self_modifying_code={},unsupported_block={},"
                               "load_delay_hazard={},unsafe_instruction_fetch={}}} "
                               "max_fallback_blocks_per_execution={}",
                               phase,
                               counters.calls,
                               counters.executedBlocks,
                               counters.executedInstructions,
                               counters.fallback.calls,
                               counters.fallback.instructions,
                               counters.fallback.compilationFailed,
                               counters.fallback.selfModifyingCode,
                               counters.fallback.unsupportedBlock,
                               counters.fallback.loadDelayHazard,
                               counters.fallback.unsafeInstructionFetch,
                               counters.fallback.refusedCalls,
                               counters.fallback.refusedCompilationFailed,
                               counters.fallback.refusedSelfModifyingCode,
                               counters.fallback.refusedUnsupportedBlock,
                               counters.fallback.refusedLoadDelayHazard,
                               counters.fallback.refusedUnsafeInstructionFetch,
                               lastExecutionFallbackPolicy.configuredMaxBlocks));
  }

  ExecutionResult fallbackThresholdFault(std::uint64_t cycles) {
    const auto &boundary = activeBoundary();
    ++counters.faults;
    reportFallbackTelemetry("threshold-exceeded", lucent::Level::Error);
    return {ExecutionExitReason::Fault,
            boundary.fallbackRefusalPc,
            cycles,
            lucent::format("Lightrec fallback refused before interpreter execution: reason={}, "
                           "admitted_blocks={}, limit={}",
                           lightrec_fallback_reason_name(boundary.fallbackRefusalReason),
                           boundary.admittedFallbackBlocks,
                           boundary.fallbackPolicy.maxBlocksPerExecution)};
  }

  Core &core;
  lightrec_ops operations{};
  std::array<std::uint8_t, kBiosSize> bios{};
  std::array<lightrec_mem_map, PSX_MAP_CODE_BUFFER + 1> maps{};
  lightrec_state *state = nullptr;
  bool initializationAttempted = false;
  FallbackPolicyProvider fallbackPolicyProvider = defaultFallbackPolicy;
  FallbackPolicy lastExecutionFallbackPolicy = defaultFallbackPolicy();
  const std::uint64_t boundaryOwnerId = nextBoundaryOwnerId();
  ExecutorCounters counters;
  StoreObserverCallback storeCallback = nullptr;
  void *storeContext = nullptr;
  StoreObserverReport storeReport{};
};

LightrecExecutor::LightrecExecutor(Core &core, FallbackPolicyProvider fallbackPolicyProvider)
    : impl_(std::make_unique<Impl>(core, fallbackPolicyProvider ? fallbackPolicyProvider : defaultFallbackPolicy)) {}

LightrecExecutor::~LightrecExecutor() {
  reportFallbackTelemetry("shutdown");
}

namespace {

std::uint64_t executedInstructionCount(const lightrec_execution_stats &stats) {
  return stats.executed_instructions + stats.fallback_instructions;
}

void accountExecutedInstructions(Core &core, std::uint64_t instructions) {
  while (instructions != 0) {
    const auto chunk =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(instructions, std::numeric_limits<std::uint32_t>::max()));
    accountGuestInstructions(core, chunk);
    instructions -= chunk;
  }
}

} // namespace

ExecutionResult LightrecExecutor::executeWithBoundary(std::uint32_t guestAddress,
                                                      std::optional<std::uint32_t> returnAddress,
                                                      bool dispatchHostServices,
                                                      ExecutionBudget budget) {
  Impl &impl = *impl_;
  ++impl.counters.calls;
  if (!impl.ensureInitialized()) {
    ++impl.counters.faults;
    return {ExecutionExitReason::Fault, guestAddress, 0, "Lightrec initialization failed"};
  }

  const FallbackPolicy fallbackPolicy = impl.fallbackPolicyProvider();
  impl.lastExecutionFallbackPolicy = fallbackPolicy;
  if (!fallbackPolicy.valid) {
    ++impl.counters.faults;
    impl.reportFallbackTelemetry("invalid-policy", lucent::Level::Error);
    return {ExecutionExitReason::Fault,
            guestAddress,
            0,
            lucent::format("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT={} is invalid; expected a non-negative integer",
                           fallbackPolicy.configuredMaxBlocks)};
  }

  Impl::BoundarySession session(impl, returnAddress, dispatchHostServices, fallbackPolicy);
  Impl::BoundaryContext &boundary = impl.activeBoundary();
  std::uint64_t consumedCycles = 0;
  std::uint64_t hostDispatches = 0;
  std::uint32_t nextPc = guestAddress;
  while (consumedCycles < budget.cycles) {
    boundary.reason = LightrecExecutor::Impl::BoundaryReason::None;
    boundary.pc = 0;
    impl.copyCoreToLightrec();
    lightrec_reset_cycle_count(impl.state, 0);
    const lightrec_execution_stats before = *lightrec_get_execution_stats(impl.state);
    nextPc =
        lightrec_execute(impl.state, nextPc, targetCycle(ExecutionBudget::fromCycles(budget.cycles - consumedCycles)));
    const std::uint64_t segmentCycles = lightrec_current_cycle_count(impl.state);
    consumedCycles += segmentCycles;
    impl.copyLightrecToCore(nextPc);
    const lightrec_execution_stats after = *lightrec_get_execution_stats(impl.state);
    impl.updateCounters(after);
    if (impl.storeReport.armed) {
      impl.storeReport.executedJitInstructions += after.executed_instructions - before.executed_instructions;
      impl.storeReport.fallbackInstructions += after.fallback_instructions - before.fallback_instructions;
    }
    if (impl.core.game) {
      accountExecutedInstructions(impl.core, executedInstructionCount(after) - executedInstructionCount(before));
    }

    const std::uint32_t flags = lightrec_exit_flags(impl.state);
    if (flags & LIGHTREC_EXIT_FALLBACK_REFUSED) {
      return impl.fallbackThresholdFault(consumedCycles);
    }
    if (flags & LIGHTREC_EXIT_OBSERVER_UNSUPPORTED) {
      ++impl.counters.faults;
      return {ExecutionExitReason::Fault,
              nextPc,
              consumedCycles,
              "Lightrec selected-store observer rejected unsupported translated PC"};
    }
    if (auto requested = impl.core.executionControl().consume()) {
      requested->cycles += consumedCycles;
      requested->guestPc = impl.core.pc;
      return *requested;
    }
    if (flags & (LIGHTREC_EXIT_SEGFAULT | LIGHTREC_EXIT_NOMEM | LIGHTREC_EXIT_UNKNOWN_OP)) {
      ++impl.counters.faults;
      return {ExecutionExitReason::Fault, nextPc, consumedCycles, "Lightrec execution fault"};
    }
    if (flags & LIGHTREC_EXIT_EXCEPTION_DELAY_SLOT) {
      ++impl.counters.faults;
      return {ExecutionExitReason::Fault, nextPc, consumedCycles, "delay-slot exception continuation is unsupported"};
    }
    if (flags & LIGHTREC_EXIT_SYSCALL) {
      const std::uint32_t instruction = impl.core.mem_r32(nextPc);
      const auto syscall = handleSyscall(impl.core, (instruction >> 6u) & 0xfffffu, nextPc);
      if (syscall != SyscallResult::Handled) {
        ++impl.counters.faults;
        return {ExecutionExitReason::Fault,
                nextPc,
                consumedCycles,
                syscall == SyscallResult::UnsupportedSelector ? "unsupported syscall selector"
                                                              : "syscall without game context"};
      }
      impl.core.pc = nextPc + 4u;
      if (dispatchHostServices) {
        nextPc = impl.core.pc;
        continue;
      }
      return {ExecutionExitReason::InterruptOrException, impl.core.pc, consumedCycles, "syscall"};
    }
    if (flags & LIGHTREC_EXIT_BREAK) {
      const std::uint32_t instruction = impl.core.mem_r32(nextPc);
      handleBreak(impl.core, (instruction >> 6u) & 0xfffffu);
      impl.core.pc = nextPc + 4u;
      return {ExecutionExitReason::HostService, impl.core.pc, consumedCycles, "break"};
    }
    if (flags & LIGHTREC_EXIT_BLOCK_BOUNDARY) {
      switch (boundary.reason) {
      case LightrecExecutor::Impl::BoundaryReason::GuestReturn:
        return {ExecutionExitReason::GuestReturn, boundary.pc, consumedCycles, "guest return"};
      case LightrecExecutor::Impl::BoundaryReason::HostDispatch: {
        if (hostDispatches >= budget.maxHostDispatches) {
          return {ExecutionExitReason::BudgetExhausted, boundary.pc, consumedCycles, "host dispatch budget exhausted"};
        }
        ++hostDispatches;
        ++impl.counters.hostDispatches;
        ExecutionResult result = dispatchGuestHostService(impl.core, boundary.pc);
        result.cycles += consumedCycles;
        if (!result.returned()) {
          return result;
        }
        // Nested native calls restore their C++ caller's scoped PC; the result owns the guest continuation.
        nextPc = result.guestPc;
        continue;
      }
      case LightrecExecutor::Impl::BoundaryReason::PendingWork:
        servicePendingWork(impl.core);
        if (auto requested = impl.core.executionControl().consume()) {
          requested->cycles += consumedCycles;
          requested->guestPc = impl.core.pc;
          return *requested;
        }
        if (!dispatchHostServices) {
          return {ExecutionExitReason::HostService, impl.core.pc, consumedCycles, "pending work"};
        }
        boundary.skipPendingBoundaryOnce = __atomic_load_n(&impl.core.pending_work, __ATOMIC_RELAXED) != 0;
        nextPc = impl.core.pc;
        continue;
      case LightrecExecutor::Impl::BoundaryReason::None:
        ++impl.counters.faults;
        return {ExecutionExitReason::Fault, nextPc, consumedCycles, "unclassified Lightrec boundary stop"};
      }
    }
    if (flags & LIGHTREC_EXIT_CHECK_INTERRUPT) {
      if (impl.core.game) {
        servicePendingWork(impl.core);
      }
      if (auto requested = impl.core.executionControl().consume()) {
        requested->cycles += consumedCycles;
        requested->guestPc = impl.core.pc;
        return *requested;
      }
      if (dispatchHostServices) {
        nextPc = impl.core.pc;
        continue;
      }
      return {ExecutionExitReason::HostService, nextPc, consumedCycles, "pending work"};
    }
    return {ExecutionExitReason::BudgetExhausted, nextPc, consumedCycles, "cycle budget exhausted"};
  }
  return {ExecutionExitReason::BudgetExhausted, nextPc, consumedCycles, "cycle budget exhausted"};
}

ExecutionResult LightrecExecutor::execute(std::uint32_t guestAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, std::nullopt, false, budget);
}

ExecutionResult LightrecExecutor::executeUntilExit(std::uint32_t guestAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, std::nullopt, true, budget);
}

ExecutionResult
LightrecExecutor::executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, returnAddress, true, budget);
}

void LightrecExecutor::requestStop() {
  if (impl_->state) {
    lightrec_set_exit_flags(impl_->state, LIGHTREC_EXIT_CHECK_INTERRUPT);
  }
}

void LightrecExecutor::invalidate(GuestAddressRange range) {
  ++impl_->counters.invalidations;
  if (impl_->state && range.end > range.begin) {
    lightrec_invalidate(impl_->state, range.begin, range.end - range.begin);
  }
}

void LightrecExecutor::invalidateAll() {
  ++impl_->counters.invalidations;
  if (impl_->state) {
    lightrec_invalidate_all(impl_->state);
  }
}

StoreObserverStatus LightrecExecutor::configureStoreObserver(std::span<const std::uint32_t> targets,
                                                             StoreObserverCallback callback,
                                                             void *context) {
  Impl &impl = *impl_;
  if (targets.empty() && (callback || context)) {
    return StoreObserverStatus::InvalidConfiguration;
  }
  if (!impl.ensureInitialized()) {
    return StoreObserverStatus::InitializationFailed;
  }
  const int result = lightrec_set_store_observer(impl.state,
                                                 targets.empty() ? nullptr : targets.data(),
                                                 targets.size(),
                                                 callback ? Impl::observeStore : nullptr,
                                                 callback ? &impl : nullptr);
  if (result == -EINVAL) {
    return StoreObserverStatus::InvalidConfiguration;
  }
  if (result == -EBUSY) {
    return StoreObserverStatus::Busy;
  }
  if (result != 0) {
    return StoreObserverStatus::InternalFailure;
  }
  if (targets.empty()) {
    impl.storeReport.armed = false;
    impl.storeCallback = nullptr;
    impl.storeContext = nullptr;
    return StoreObserverStatus::Configured;
  }
  impl.storeReport = {};
  impl.storeReport.targetCount = targets.size();
  impl.storeReport.armed = true;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    impl.storeReport.targets[i].guestPc = targets[i];
  }
  impl.storeCallback = callback;
  impl.storeContext = context;
  return StoreObserverStatus::Configured;
}

StoreObserverReport LightrecExecutor::storeObserverReport() const {
  return impl_->storeReport;
}

const ExecutorCounters &LightrecExecutor::counters() const {
  return impl_->counters;
}

void LightrecExecutor::reportFallbackTelemetry(std::string_view phase) const {
  const lucent::Level level = impl_->counters.fallback.calls == 0 && impl_->counters.fallback.refusedCalls == 0
                                  ? lucent::Level::Info
                                  : lucent::Level::Warn;
  impl_->reportFallbackTelemetry(phase, level);
}

bool LightrecExecutor::available() const {
  return !impl_->initializationAttempted || impl_->state != nullptr;
}

} // namespace psx::cpu
