#include "lightrec_executor.h"

#include "core.h"
#include "execution_control.h"
#include "execution_ledger.h"
#include "execution_services.h"
#include "function_reach.h"
#include "game.h"
#include "gte_register_transfer.h"
#include "guest_memory_map.h"
#include "native_dispatch.h"
#include "segment_clock.h"
#include "side_effect_journal.h"
#include "store_observe.h"

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
#include <thread>

namespace psx::cpu {
namespace {

static_assert(kMaxObservedStoreTargets == LIGHTREC_STORE_OBSERVER_TARGETS);

std::uint32_t targetCycle(ExecutionBudget budget) {
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(budget.cycles, std::numeric_limits<std::uint32_t>::max()));
}

// Only the hardware-register window reaches io_read/io_write, the one place a guest observes elapsed time.
bool isDeviceAddress(std::uint32_t address) {
  return address >= kHardwareBase && address < kHardwareBase + kHardwareBytes;
}

// Lightrec state is per lightrec_state; the cap only bounds a leak of machines.
constexpr int kMaxLiveMachines = 8;
std::atomic<int> liveMachines{0};

std::uint64_t executedInstructionCount(const lightrec_execution_stats &stats) {
  return stats.executed_instructions + stats.fallback_instructions;
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
        .context = this,
    };
    initializeMaps();
  }

  bool ensureInitialized() {
    std::scoped_lock lifecycleLock(lifecycleMutex_);
    if (state || initializationAttempted) {
      return state != nullptr;
    }
    initializationAttempted = true;
    const int live = liveMachines.fetch_add(1);
    if (live >= kMaxLiveMachines) {
      liveMachines.fetch_sub(1);
      lucent::error(
          "executor", "refusing to initialize a {}. Lightrec machine in one process ({} already live)", live + 1, live);
      return false;
    }
    char programName[] = "psxport";
    state = lightrec_init(programName, maps.data(), maps.size(), &operations);
    if (!state) {
      liveMachines.fetch_sub(1);
      lucent::error("executor", "Lightrec initialization failed");
      return false;
    }
    return true;
  }

  ~Impl() {
    if (!state) {
      return;
    }
    std::scoped_lock lifecycleLock(lifecycleMutex_);
    lightrec_destroy(state);
    state = nullptr;
    liveMachines.fetch_sub(1);
  }

  // The Lightrec state carries this object as its context.
  static Impl &owner(lightrec_state *lightrec) {
    return *static_cast<Impl *>(lightrec_get_context(lightrec));
  }

  static void storeByte(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    impl.core.mem_w8(address, static_cast<std::uint8_t>(value), ExecutableWriteSource::Cpu);
  }

  static void storeHalf(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    impl.core.mem_w16(address, static_cast<std::uint16_t>(value), ExecutableWriteSource::Cpu);
  }

  static void storeWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    impl.core.mem_w32(address, value, ExecutableWriteSource::Cpu);
  }

  static void
  storeUnalignedWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address, std::uint32_t value) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    impl.core.mem_w32(address, value, ExecutableWriteSource::Cpu);
  }

  static std::uint8_t loadByte(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    return impl.core.mem_r8(address);
  }

  static std::uint16_t loadHalf(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    return impl.core.mem_r16(address);
  }

  static std::uint32_t loadWord(lightrec_state *lightrec, std::uint32_t, void *, std::uint32_t address) {
    Impl &impl = owner(lightrec);
    impl.commitDeviceClock(lightrec, address);
    return impl.core.mem_r32(address);
  }

  static void cop2Operation(lightrec_state *lightrec, std::uint32_t opcode) {
    Impl &impl = owner(lightrec);
    lightrec_registers *registers = lightrec_get_registers(lightrec);
    psx::cpu::importGteRegisters(impl.core.gte().registers(), registers->cp2d, registers->cp2c);
    impl.core.gte().execute(opcode);
    psx::cpu::exportGteRegisters(impl.core.gte().registers(), registers->cp2d, registers->cp2c);
  }

  static void enableRam(lightrec_state *, bool) {
    // Core owns one coherent RAM image and does not expose a separate cache-isolation byte array.
  }

  // Commits the guest time since the last device access so the device about to be read sees it move.
  // It never re-enters a memory callback, and leaves the host turn and spin sampling to the segment end.
  void commitDeviceClock(lightrec_state *lightrec, std::uint32_t address) {
    ++counters.memoryCallbacks;
    if (core.game == nullptr || !isDeviceAddress(address)) {
      return;
    }
    // The override differential's shadow path holds guest time; its device reads are replayed.
    if (core.sideEffectJournal != nullptr && core.sideEffectJournal->withholdsGuestTime()) {
      return;
    }
    const std::uint32_t instructions = segmentClock.commitThrough(lightrec_current_cycle_count(lightrec));
    if (instructions == 0) {
      return;
    }
    ++counters.deviceClockCommits;
    counters.deviceClockCommitInstructions += instructions;
    core.game->timing.advanceGuestInstructionTicks(instructions);
  }

  static constexpr lightrec_mem_map_ops kMemoryOps{
      .sb = storeByte,
      .sh = storeHalf,
      .sw = storeWord,
      .lb = loadByte,
      .lh = loadHalf,
      .lw = loadWord,
      .lwu = loadWord,
      .swu = storeUnalignedWord,
  };

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
    std::thread::id thread{};
    bool active = false;
  };

  // Host threads inside the executor at once: the main thread and the fibers parked inside it.
  static constexpr std::size_t kBoundarySlots = 8;

  BoundaryContext *boundaryOf(std::thread::id thread) {
    for (BoundaryContext &slot : boundaries) {
      if (slot.thread == thread) {
        return &slot;
      }
    }
    return nullptr;
  }

  // The boundary this host thread entered; a cooperative task thread holds its own while it is parked.
  BoundaryContext &activeBoundary() {
    const std::thread::id thread = std::this_thread::get_id();
    if (cachedBoundary == nullptr || cachedBoundary->thread != thread) {
      cachedBoundary = boundaryOf(thread);
    }
    if (cachedBoundary == nullptr || !cachedBoundary->active) {
      lucent::error("executor", "Lightrec callback reached without a boundary on its host thread");
      std::abort();
    }
    return *cachedBoundary;
  }

  static lightrec_fallback_action
  fallbackAdmission(lightrec_state *, const lightrec_fallback_event *event, void *userData) {
    auto &impl = *static_cast<Impl *>(userData);
    auto &boundary = impl.activeBoundary();
    // A cross-block load-delay hazard interprets at most three instructions, so the block limit does not apply.
    if (event->reason == LIGHTREC_FALLBACK_LOAD_DELAY_HAZARD) {
      return LIGHTREC_FALLBACK_ALLOW;
    }
    if (boundary.admittedFallbackBlocks < boundary.fallbackPolicy.maxBlocksPerExecution) {
      ++boundary.admittedFallbackBlocks;
      return LIGHTREC_FALLBACK_ALLOW;
    }
    boundary.fallbackRefusalPc = event->guest_pc;
    boundary.fallbackRefusalReason = event->reason;
    return LIGHTREC_FALLBACK_REFUSE;
  }

  static ResolvedStoreTarget resolveStore(std::uint32_t instruction, std::span<const std::uint32_t, 34> gpr) {
    const std::uint32_t low = instruction & 0xFFFFu;
    ResolvedStoreTarget resolved;
    resolved.baseRegister = (instruction >> 21) & 0x1Fu;
    resolved.sourceRegister = (instruction >> 16) & 0x1Fu;
    resolved.displacement = static_cast<std::int32_t>((low ^ 0x8000u) - 0x8000u);
    resolved.address = gpr[resolved.baseRegister] + static_cast<std::uint32_t>(resolved.displacement);
    resolved.value = gpr[resolved.sourceRegister];
    resolved.valid = true;
    return resolved;
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
      const std::uint32_t instruction = impl.core.mem_r32(guestPc);
      const ResolvedStoreTarget resolved = resolveStore(instruction, registers->gpr);
      if (phase == LIGHTREC_STORE_BEFORE) {
        ++target.before;
        if (target.lastTarget.valid && target.lastTarget.address != resolved.address) {
          ++target.distinctTargets;
        } else if (!target.lastTarget.valid) {
          target.firstTarget = resolved;
        }
        target.previousTarget = target.lastTarget;
        target.lastTarget = resolved;
      } else {
        ++target.after;
      }
      const StoreObservation observation{
          guestPc,
          phase == LIGHTREC_STORE_BEFORE ? StoreObservationPhase::Before : StoreObservationPhase::After,
          cycle,
          instruction,
          resolved,
          std::span(registers->gpr),
          std::span(registers->cp0),
          std::span(registers->cp2d),
          std::span(registers->cp2c),
      };
      ++impl.storeReport.observations;
      impl.storeCallback(observation, impl.storeContext);
      return;
    }
    // An unarmed store in an armed block: Lightrec instruments whole blocks.
    ++impl.storeReport.unrequestedObservations;
  }

  static lightrec_block_boundary_action
  blockBoundary(lightrec_state *, std::uint32_t guestPc, std::uint32_t *, void *userData) {
    auto &impl = *static_cast<Impl *>(userData);
    if (impl.reach) {
      impl.reach->observe(guestPc);
    }
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
        impl.core.nativeDispatcher().classify(guestPc) != GuestHostDispatchKind::ExecuteGuest) {
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
        : boundary_(claim(impl, std::this_thread::get_id())), previous_(boundary_) {
      boundary_ = {.returnAddress = returnAddress,
                   .dispatchHostServices = dispatchHostServices,
                   .fallbackPolicy = fallbackPolicy,
                   .thread = previous_.thread,
                   .active = true};
    }

    ~BoundarySession() {
      boundary_ = previous_.active ? previous_ : BoundaryContext{};
    }

    BoundarySession(const BoundarySession &) = delete;
    BoundarySession &operator=(const BoundarySession &) = delete;

  private:
    static BoundaryContext &claim(Impl &impl, std::thread::id thread) {
      BoundaryContext *slot = impl.boundaryOf(thread);
      if (slot == nullptr) {
        slot = impl.boundaryOf(std::thread::id{});
        if (slot == nullptr) {
          lucent::error("executor", "more than {} host threads entered the executor at once", kBoundarySlots);
          std::abort();
        }
        slot->thread = thread;
      }
      impl.cachedBoundary = slot;
      return *slot;
    }

    BoundaryContext &boundary_;
    BoundaryContext previous_;
  };

  inline static std::mutex lifecycleMutex_;

  void initializeMaps() {
    maps.fill({});
    maps[PSX_MAP_KERNEL_USER_RAM] = {
        .pc = 0,
        .length = kMainRamBytes,
        .address = core.ram,
        .ops = &kMemoryOps,
    };
    maps[PSX_MAP_BIOS] = {
        .pc = kBiosBase,
        .length = kBiosBytes,
        .address = bios.data(),
    };
    maps[PSX_MAP_SCRATCH_PAD] = {
        .pc = kScratchpadPhysicalBase,
        .length = kScratchpadBytes,
        .address = core.scratch,
        .ops = &kMemoryOps,
    };
    maps[PSX_MAP_HW_REGISTERS] = {
        .pc = kHardwareBase,
        .length = kHardwareBytes,
        .address = core.ram,
        .ops = &kMemoryOps,
    };
    maps[PSX_MAP_MIRROR1] = {
        .pc = 1u * kMainRamBytes,
        .length = kMainRamBytes,
        .mirror_of = &maps[PSX_MAP_KERNEL_USER_RAM],
    };
    maps[PSX_MAP_MIRROR2] = {
        .pc = 2u * kMainRamBytes,
        .length = kMainRamBytes,
        .mirror_of = &maps[PSX_MAP_KERNEL_USER_RAM],
    };
    maps[PSX_MAP_MIRROR3] = {
        .pc = 3u * kMainRamBytes,
        .length = kMainRamBytes,
        .mirror_of = &maps[PSX_MAP_KERNEL_USER_RAM],
    };
  }

  void copyCoreToLightrec() {
    lightrec_registers *registers = lightrec_get_registers(state);
    std::copy_n(core.r, 32, registers->gpr);
    registers->gpr[32] = core.lo;
    registers->gpr[33] = core.hi;
    std::copy_n(core.cop0, 16, registers->cp0);
    core.gte().bind();
    psx::cpu::exportGteRegisters(core.gte().registers(), registers->cp2d, registers->cp2c);
  }

  void copyLightrecToCore(std::uint32_t nextPc) {
    lightrec_registers *registers = lightrec_get_registers(state);
    std::copy_n(registers->gpr, 32, core.r);
    core.r[0] = 0;
    core.lo = registers->gpr[32];
    core.hi = registers->gpr[33];
    core.pc = nextPc;
    std::copy_n(registers->cp0, 16, core.cop0);
    core.gte().bind();
    psx::cpu::importGteRegisters(core.gte().registers(), registers->cp2d, registers->cp2c);
  }

  // Classifies the resume pc a budget exit hands back; an unresolvable one is reported at once.
  static ExecutionExitReason recordBudgetExit(Impl &impl, std::uint32_t resumePc) {
    ++impl.counters.budgetExits;
    if (impl.core.imageCatalog().resolve(resumePc).has_value()) {
      ++impl.counters.budgetExitPcInCodeImage;
    } else {
      ++impl.counters.budgetExitPcOutsideCodeImage;
      lucent::error("lightrec",
                    "budget exit reported resume pc 0x{:08X}, which is NOT in any loaded code "
                    "image; guest execution will resume there. {} of {} budget exit(s) so far "
                    "reported a pc outside every code image",
                    resumePc,
                    impl.counters.budgetExitPcOutsideCodeImage,
                    impl.counters.budgetExits);
    }
    return ExecutionExitReason::BudgetExhausted;
  }

  void updateCounters(const lightrec_execution_stats &stats) {
    counters.translatedBlocks = stats.translated_blocks;
    counters.executedBlocks = stats.executed_blocks;
    counters.executedInstructions = executedInstructionCount(stats);
    counters.cacheHits = stats.cache_hits;
    counters.cacheMisses = stats.cache_misses;
    counters.lightrecInvalidationCalls = stats.invalidations;
    counters.lightrecInvalidationWords = stats.invalidation_words;
    counters.lightrecInvalidationGuards = stats.invalidation_guards;
    counters.lightrecInvalidationBlockScans = stats.invalidation_scans;
    counters.lightrecInvalidatedBlocks = stats.invalidated_blocks;
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

  ExecutionResult fallbackThresholdFault(std::uint64_t cycles) {
    const auto &boundary = activeBoundary();
    ++counters.faults;
    logLedger(counters, "threshold-exceeded", lucent::Level::Error);
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
  std::array<std::uint8_t, kBiosBytes> bios{};
  std::array<lightrec_mem_map, PSX_MAP_CODE_BUFFER + 1> maps{};
  lightrec_state *state = nullptr;
  bool initializationAttempted = false;
  FallbackPolicyProvider fallbackPolicyProvider = nullptr;
  // One boundary per host thread; strict fiber ping-pong means no two threads touch this at once.
  std::array<BoundaryContext, kBoundarySlots> boundaries{};
  BoundaryContext *cachedBoundary = nullptr;
  ExecutorCounters counters;
  SegmentClockLedger segmentClock;
  StoreObserverCallback storeCallback = nullptr;
  void *storeContext = nullptr;
  StoreObserverReport storeReport{};
  std::unique_ptr<FunctionReach> reach;
};

LightrecExecutor::LightrecExecutor(Core &core, FallbackPolicyProvider fallbackPolicyProvider)
    : impl_(std::make_unique<Impl>(core, fallbackPolicyProvider)) {}

LightrecExecutor::~LightrecExecutor() {
  const ExecutorCounters &counters = impl_->counters;
  const bool fellBack = counters.fallback.calls != 0 || counters.fallback.refusedCalls != 0;
  logLedger(counters, "shutdown", fellBack ? lucent::Level::Warn : lucent::Level::Info);
  psx::cpu::reportStoreObserver(impl_->storeReport);
}

void LightrecExecutor::attachFunctionReach(std::unique_ptr<FunctionReach> reach) {
  impl_->reach = std::move(reach);
}

namespace {

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
  impl.counters.maxFallbackBlocksPerExecution = fallbackPolicy.configuredMaxBlocks;
  if (!fallbackPolicy.valid) {
    ++impl.counters.faults;
    logLedger(impl.counters, "invalid-policy", lucent::Level::Error);
    return {ExecutionExitReason::Fault,
            guestAddress,
            0,
            lucent::format("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT={} is invalid; expected a non-negative integer",
                           fallbackPolicy.configuredMaxBlocks)};
  }

  Impl::BoundarySession session(impl, returnAddress, dispatchHostServices, fallbackPolicy);
  Impl::BoundaryContext &boundary = impl.activeBoundary();
  // Translated frames are on the stack from here; the journal must not raise its bound fault through them.
  const SideEffectJournal::TranslatedExecutionScope translated(impl.core);
  std::uint64_t consumedCycles = 0;
  std::uint64_t hostDispatches = 0;
  std::uint32_t nextPc = guestAddress;
  while (consumedCycles < budget.cycles) {
    boundary.reason = LightrecExecutor::Impl::BoundaryReason::None;
    boundary.pc = 0;
    impl.copyCoreToLightrec();
    lightrec_reset_cycle_count(impl.state, 0);
    // Zeroed together with Lightrec's counter: the ledger measures deltas against it.
    impl.segmentClock.beginSegment();
    const lightrec_execution_stats before = *lightrec_get_execution_stats(impl.state);
    // A segment also ends at the host field deadline and the next device deadline, which are only
    // folded into the clock between segments.
    std::uint64_t segmentCycleBudget = budget.cycles - consumedCycles;
    if (impl.core.game) {
      const std::uint64_t untilFieldDue = impl.core.hostTurn.ticksUntilDue(impl.core);
      if (untilFieldDue != 0) {
        segmentCycleBudget = std::min(segmentCycleBudget, untilFieldDue);
      }
      const std::optional<std::uint64_t> untilDeviceEvent = impl.core.game->timing.ticksUntilDeviceEvent();
      if (untilDeviceEvent && *untilDeviceEvent != 0) {
        segmentCycleBudget = std::min(segmentCycleBudget, *untilDeviceEvent);
      }
    }
    // lightrec_execute overwrites nextPc with the exit pc; the start names the block behind a wild target.
    const std::uint32_t blockStart = nextPc;
    nextPc = lightrec_execute(impl.state, nextPc, targetCycle(ExecutionBudget::fromCycles(segmentCycleBudget)));
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
      // The exact instruction count, less what device-access commits already charged.
      accountExecutedInstructions(
          impl.core, impl.segmentClock.uncommitted(executedInstructionCount(after) - executedInstructionCount(before)));
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
    if (auto requested = impl.core.executionControl().consume(impl.core.pc, consumedCycles)) {
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
      const auto syscall = handleSyscall(impl.core, nextPc);
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
          return {
              Impl::recordBudgetExit(impl, boundary.pc), boundary.pc, consumedCycles, "host dispatch budget exhausted"};
        }
        ++hostDispatches;
        ++impl.counters.hostDispatches;
        ExecutionResult result = dispatchGuestHostService(impl.core, boundary.pc);
        result.cycles += consumedCycles;
        if (!result.returned()) {
          if (result.reason == ExecutionExitReason::Fault) {
            lucent::error("executor",
                          "host dispatch to 0x{:08X} FAILED ({}); the block that produced this target began at "
                          "0x{:08X}",
                          boundary.pc,
                          result.detail,
                          blockStart);
          }
          return result;
        }
        // Nested native calls restore their C++ caller's scoped PC; the result owns the guest continuation.
        nextPc = result.guestPc;
        continue;
      }
      case LightrecExecutor::Impl::BoundaryReason::PendingWork:
        servicePendingWork(impl.core);
        if (auto requested = impl.core.executionControl().consume(impl.core.pc, consumedCycles)) {
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
      if (auto requested = impl.core.executionControl().consume(impl.core.pc, consumedCycles)) {
        return *requested;
      }
      if (dispatchHostServices) {
        nextPc = impl.core.pc;
        continue;
      }
      return {ExecutionExitReason::HostService, nextPc, consumedCycles, "pending work"};
    }
    if (consumedCycles < budget.cycles) {
      // Ended at the field or device deadline, not the caller's budget.
      continue;
    }
    return {Impl::recordBudgetExit(impl, nextPc), nextPc, consumedCycles, "cycle budget exhausted"};
  }
  return {ExecutionExitReason::BudgetExhausted, nextPc, consumedCycles, "cycle budget exhausted"};
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

void LightrecExecutor::invalidate(GuestAddressRange range, ExecutableWriteSource source) {
  ++impl_->counters.invalidations;
  ++impl_->counters.invalidationsBySource[static_cast<std::size_t>(source)];
  if (impl_->state && range.end > range.begin) {
    lightrec_invalidate(impl_->state, range.begin, range.end - range.begin);
  }
}

void LightrecExecutor::invalidateAll(ExecutableWriteSource source) {
  ++impl_->counters.invalidations;
  ++impl_->counters.invalidationsBySource[static_cast<std::size_t>(source)];
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

bool LightrecExecutor::available() const {
  return !impl_->initializationAttempted || impl_->state != nullptr;
}

} // namespace psx::cpu
