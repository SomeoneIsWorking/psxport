#include "lightrec_executor.h"

#include "core.h"
#include "execution_control.h"
#include "execution_services.h"
#include "hw_bind.h"
#include "invalidation.h"
#include "native_dispatch.h"

#include <lightrec.h>
#include <lucent/log.h>

#include <algorithm>
#include <array>
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

std::uint32_t targetCycle(ExecutionBudget budget) {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(budget.cycles, std::numeric_limits<std::uint32_t>::max()));
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
    impl.invalidateGuestStore(address, 1);
  }

  static void storeHalf(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w16(address, static_cast<std::uint16_t>(value));
    impl.invalidateGuestStore(address, 2);
  }

  static void storeWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w32(address, value);
    impl.invalidateGuestStore(address, 4);
  }

  static void
  storeUnalignedWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.core.mem_w32(address, value);
    impl.invalidateGuestStore(address, 4);
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
    for (std::uint32_t index = 0; index < 32; ++index) {
      gte_write_data(index, registers->cp2d[index]);
      gte_write_ctrl(index, registers->cp2c[index]);
    }
    gte_op_at(&impl.core, opcode, impl.core.pc);
    for (std::uint32_t index = 0; index < 32; ++index) {
      registers->cp2d[index] = gte_read_data(index);
      registers->cp2c[index] = gte_read_ctrl(index);
    }
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

  static lightrec_fallback_action
  fallbackAdmission(lightrec_state *lightrec, const lightrec_fallback_event *event, void *userData) {
    auto &impl = *static_cast<Impl *>(userData);
    const auto &stats = *lightrec_get_execution_stats(lightrec);
    const std::uint64_t admitted = stats.fallback_blocks - impl.fallbackBaselineBlocks;
    if (admitted < impl.fallbackPolicy.maxBlocksPerExecution) {
      return LIGHTREC_FALLBACK_ALLOW;
    }
    impl.fallbackRefusalPc = event->guest_pc;
    impl.fallbackRefusalReason = event->reason;
    return LIGHTREC_FALLBACK_REFUSE;
  }

  static lightrec_block_boundary_action
  blockBoundary(lightrec_state *, std::uint32_t guestPc, std::uint32_t *, void *userData) {
    auto &impl = *static_cast<Impl *>(userData);
    if (impl.returnAddress && guestPc == *impl.returnAddress) {
      impl.boundaryReason = BoundaryReason::GuestReturn;
    } else if (impl.skipPendingBoundaryOnce) {
      impl.skipPendingBoundaryOnce = false;
    } else if (impl.core.game && impl.core.active_native_address == 0 && impl.core.pending_guest_redirect == 0 &&
               __atomic_load_n(&impl.core.pending_work, __ATOMIC_RELAXED) != 0) {
      impl.boundaryReason = BoundaryReason::PendingWork;
    }
    if (impl.boundaryReason == BoundaryReason::None && impl.dispatchHostServices &&
        classifyGuestHostDispatch(impl.core, guestPc) != GuestHostDispatchKind::ExecuteGuest) {
      impl.boundaryReason = BoundaryReason::HostDispatch;
    }
    if (impl.boundaryReason == BoundaryReason::None) {
      return LIGHTREC_BLOCK_CONTINUE;
    }
    impl.boundaryPc = guestPc;
    return LIGHTREC_BLOCK_STOP;
  }

  class BoundarySession {
  public:
    BoundarySession(Impl &impl,
                    std::optional<std::uint32_t> returnAddress,
                    bool dispatchHostServices,
                    FallbackPolicy fallbackPolicy)
        : impl_(impl), previousReturnAddress_(impl.returnAddress), previousDispatch_(impl.dispatchHostServices),
          previousSkipPending_(impl.skipPendingBoundaryOnce), previousReason_(impl.boundaryReason),
          previousPc_(impl.boundaryPc), previousFallbackPolicy_(impl.fallbackPolicy),
          previousFallbackBaselineBlocks_(impl.fallbackBaselineBlocks),
          previousFallbackRefusalPc_(impl.fallbackRefusalPc),
          previousFallbackRefusalReason_(impl.fallbackRefusalReason) {
      impl_.returnAddress = returnAddress;
      impl_.dispatchHostServices = dispatchHostServices;
      impl_.skipPendingBoundaryOnce = false;
      impl_.boundaryReason = BoundaryReason::None;
      impl_.boundaryPc = 0;
      impl_.fallbackPolicy = fallbackPolicy;
      impl_.fallbackBaselineBlocks = lightrec_get_execution_stats(impl_.state)->fallback_blocks;
      impl_.fallbackRefusalPc = 0;
      impl_.fallbackRefusalReason = LIGHTREC_FALLBACK_NONE;
    }

    ~BoundarySession() {
      impl_.returnAddress = previousReturnAddress_;
      impl_.dispatchHostServices = previousDispatch_;
      impl_.skipPendingBoundaryOnce = previousSkipPending_;
      impl_.boundaryReason = previousReason_;
      impl_.boundaryPc = previousPc_;
      impl_.fallbackPolicy = previousFallbackPolicy_;
      impl_.fallbackBaselineBlocks = previousFallbackBaselineBlocks_;
      impl_.fallbackRefusalPc = previousFallbackRefusalPc_;
      impl_.fallbackRefusalReason = previousFallbackRefusalReason_;
    }

  private:
    Impl &impl_;
    std::optional<std::uint32_t> previousReturnAddress_;
    bool previousDispatch_ = false;
    bool previousSkipPending_ = false;
    BoundaryReason previousReason_ = BoundaryReason::None;
    std::uint32_t previousPc_ = 0;
    FallbackPolicy previousFallbackPolicy_{};
    std::uint64_t previousFallbackBaselineBlocks_ = 0;
    std::uint32_t previousFallbackRefusalPc_ = 0;
    lightrec_fallback_reason previousFallbackRefusalReason_ = LIGHTREC_FALLBACK_NONE;
  };

  void invalidateGuestStore(std::uint32_t address, std::uint32_t length) {
    if (address >= 0x00800000u) {
      return;
    }
    notifyExecutableWrite(core, {address, address + length}, ExecutableWriteSource::Cpu);
  }

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
    for (std::uint32_t index = 0; index < 32; ++index) {
      registers->cp2d[index] = gte_read_data(index);
      registers->cp2c[index] = gte_read_ctrl(index);
    }
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
    for (std::uint32_t index = 0; index < 32; ++index) {
      gte_write_data(index, registers->cp2d[index]);
      gte_write_ctrl(index, registers->cp2c[index]);
    }
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
    ++counters.faults;
    reportFallbackTelemetry("threshold-exceeded", lucent::Level::Error);
    return {ExecutionExitReason::Fault,
            fallbackRefusalPc,
            cycles,
            lucent::format("Lightrec fallback refused before interpreter execution: reason={}, "
                           "admitted_blocks={}, limit={}",
                           lightrec_fallback_reason_name(fallbackRefusalReason),
                           counters.fallback.calls - fallbackBaselineBlocks,
                           fallbackPolicy.maxBlocksPerExecution)};
  }

  Core &core;
  lightrec_ops operations{};
  std::array<std::uint8_t, kBiosSize> bios{};
  std::array<lightrec_mem_map, PSX_MAP_CODE_BUFFER + 1> maps{};
  lightrec_state *state = nullptr;
  bool initializationAttempted = false;
  FallbackPolicyProvider fallbackPolicyProvider = defaultFallbackPolicy;
  FallbackPolicy lastExecutionFallbackPolicy = defaultFallbackPolicy();
  std::optional<std::uint32_t> returnAddress;
  bool dispatchHostServices = false;
  bool skipPendingBoundaryOnce = false;
  BoundaryReason boundaryReason = BoundaryReason::None;
  std::uint32_t boundaryPc = 0;
  FallbackPolicy fallbackPolicy{};
  std::uint64_t fallbackBaselineBlocks = 0;
  std::uint32_t fallbackRefusalPc = 0;
  lightrec_fallback_reason fallbackRefusalReason = LIGHTREC_FALLBACK_NONE;
  ExecutorCounters counters;
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
                                                      std::uint32_t returnAddress,
                                                      bool stopAtReturn,
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

  const std::optional<std::uint32_t> returnTarget = stopAtReturn ? std::optional{returnAddress} : std::nullopt;
  Impl::BoundarySession session(impl, returnTarget, stopAtReturn, fallbackPolicy);
  std::uint64_t consumedCycles = 0;
  std::uint32_t nextPc = guestAddress;
  while (consumedCycles < budget.cycles) {
    impl.boundaryReason = LightrecExecutor::Impl::BoundaryReason::None;
    impl.boundaryPc = 0;
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
    if (impl.core.game) {
      accountExecutedInstructions(impl.core, executedInstructionCount(after) - executedInstructionCount(before));
    }

    const std::uint32_t flags = lightrec_exit_flags(impl.state);
    if (flags & LIGHTREC_EXIT_FALLBACK_REFUSED) {
      return impl.fallbackThresholdFault(consumedCycles);
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
    if (flags & LIGHTREC_EXIT_SYSCALL) {
      const std::uint32_t instruction = impl.core.mem_r32(nextPc);
      handleSyscall(impl.core, (instruction >> 6u) & 0xfffffu, nextPc);
      impl.core.pc = nextPc + 4u;
      return {ExecutionExitReason::InterruptOrException, impl.core.pc, consumedCycles, "syscall"};
    }
    if (flags & LIGHTREC_EXIT_BREAK) {
      const std::uint32_t instruction = impl.core.mem_r32(nextPc);
      handleBreak(impl.core, (instruction >> 6u) & 0xfffffu);
      impl.core.pc = nextPc + 4u;
      return {ExecutionExitReason::HostService, impl.core.pc, consumedCycles, "break"};
    }
    if (flags & LIGHTREC_EXIT_BLOCK_BOUNDARY) {
      switch (impl.boundaryReason) {
      case LightrecExecutor::Impl::BoundaryReason::GuestReturn:
        return {ExecutionExitReason::GuestReturn, impl.boundaryPc, consumedCycles, "guest return"};
      case LightrecExecutor::Impl::BoundaryReason::HostDispatch: {
        ExecutionResult result = dispatchGuestHostService(impl.core, impl.boundaryPc);
        result.cycles += consumedCycles;
        if (!result.returned()) {
          return result;
        }
        nextPc = impl.core.pc;
        continue;
      }
      case LightrecExecutor::Impl::BoundaryReason::PendingWork:
        servicePendingWork(impl.core);
        if (auto requested = impl.core.executionControl().consume()) {
          requested->cycles += consumedCycles;
          requested->guestPc = impl.core.pc;
          return *requested;
        }
        if (!stopAtReturn) {
          return {ExecutionExitReason::HostService, impl.core.pc, consumedCycles, "pending work"};
        }
        impl.skipPendingBoundaryOnce = __atomic_load_n(&impl.core.pending_work, __ATOMIC_RELAXED) != 0;
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
      if (stopAtReturn) {
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
  return executeWithBoundary(guestAddress, 0, false, budget);
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
