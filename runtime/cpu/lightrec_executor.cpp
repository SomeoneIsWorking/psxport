#include "lightrec_executor.h"

#include "core.h"
#include "execution_control.h"
#include "execution_services.h"
#include "function_reach.h"
#include "game.h"
#include "gte_register_transfer.h"
#include "host_turn.h"
#include "hw_bind.h"
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

// Is this address a DEVICE access? The hardware-register map is the one the executor itself installs
// above (`kHardwareBase`/`kHardwareSize`), and it is exactly the set of addresses `Core::host_ptr`
// does not resolve, so every access in it reaches `Core::io_read`/`io_write`. That is the seam the
// executor contract names ("an HLE/device callback ... is observed by host code"), and it is the only
// place a guest can observe elapsed time.
//
// The other out-of-line accesses — an unresolvable RAM or scratchpad address, which reaches the same
// callbacks because those maps carry ops too — are NOT device accesses, and charging them would tax
// the JIT's hottest memory path for state no guest can read.
bool isDeviceAddress(std::uint32_t address) {
  return address >= kHardwareBase && address < kHardwareBase + kHardwareSize;
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
    gte_bind(&impl.core);
    gte_import_registers(impl.core.game->gte, registers->cp2d, registers->cp2c);
    gte_op_at(&impl.core, opcode, impl.core.pc);
    gte_export_registers(impl.core.game->gte, registers->cp2d, registers->cp2c);
  }

  static void enableRam(lightrec_state *, bool) {
    // Core owns one coherent RAM image and does not expose a separate cache-isolation byte array.
  }

  // Commit the guest time elapsed since the last device access, so the device about to be observed
  // reads a clock that has moved. See `SegmentClockLedger` for the delta and for why it cannot run
  // backwards; this is the call site that feeds it Lightrec's live cycle count.
  //
  // IT CANNOT RE-ENTER. Nothing reachable from here calls back into the Lightrec memory callbacks:
  // `Timing::advanceGuestInstructionTicks` reaches the SIO service and `serviceCdc`, and
  // `cdc_drive_service` decodes a sector or executes a command entirely in host state. The memory
  // callbacks are invoked by generated code, not by `Core::mem_r*`/`mem_w*`, so host code calling
  // those cannot come back here. (Scanned 2026-09-28: every `dispatchGuest*`/`executeFunction` call
  // site in `runtime/` is a function-entry, interrupt, scheduler or task boundary — none is
  // reachable from `Core::io_read`/`io_write`.)
  //
  // IT DELIBERATELY DOES NOT RAISE THE HOST TURN OR SAMPLE THE SPIN DETECTOR, which is what the
  // segment-boundary `accountGuestInstructions` does beyond the clock. A host turn is TAKEN at an
  // eligible boundary (`serviceHostTurn` refuses mid-critical-section states), and raising the
  // request here would only make the block-boundary callback end this segment early for it. The
  // spin detector samples `Core::pc`, and inside a segment that is the PC the segment ENTERED at —
  // the architectural PC is synchronized back only after `lightrec_execute` returns — so sampling it
  // here would pin the anchor to a stale address. Both still run, unchanged, at the segment boundary
  // where their inputs are real.
  void commitDeviceClock(lightrec_state *lightrec, std::uint32_t address) {
    // Every memory callback psxport received, device or not. This is the DENOMINATOR the charge
    // count is meaningless without: `deviceClockCommits == 0` on a run that made no memory callback
    // at all and on a run that made many and charged none are different facts, and only this number
    // tells them apart. It is also what makes the address gate testable — a gate that rejected
    // everything would report 0/0 here and look identical to a run that never reached the callbacks.
    ++counters.memoryCallbacks;
    if (core.game == nullptr || !isDeviceAddress(address)) {
      return;
    }
    // The override differential's shadow path runs with guest time held (side_effect_journal.h);
    // its device reads are replayed, so no device can observe the clock it would have committed.
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
    // A cross-block load-delay hazard is architected R3000 behaviour, not a refused compilation:
    // a taken branch whose delay slot loads $r, into a block whose FIRST instruction reads $r, must
    // see $r's old value. Lightrec resolves it by interpreting exactly that instruction (plus a
    // branch and its delay slot when the first instruction is itself a branch), so the interpreted
    // work per event is at most three guest instructions regardless of the block. Hand-scheduled
    // GTE clipping loops hit it once per vertex, so a per-call BLOCK limit cannot bound it and would
    // fault correct guest code (Toy Story 2 at 0x800202F0). It is therefore exempt from the block
    // limit and reported by its own counters (fallback_instructions against executed_instructions).
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

  // The load/store destination, derived from the instruction word and the register file. See
  // `ResolvedStoreTarget` for why the sign extension is the whole ball game.
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
      if (phase == LIGHTREC_STORE_BEFORE) {
        ++target.before;
        const ResolvedStoreTarget resolved = resolveStore(impl.core.mem_r32(guestPc), registers->gpr);
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
          impl.core.mem_r32(guestPc),
          std::span(registers->gpr),
          std::span(registers->cp0),
          std::span(registers->cp2d),
          std::span(registers->cp2c),
      };
      impl.storeCallback(observation, impl.storeContext);
      return;
    }
    // A store in an INSTRUMENTED BLOCK whose PC nobody asked for. This used to `std::abort()`, on the
    // reasoning that a translated callback for an unregistered PC violates the per-state contract. That
    // reasoning is WRONG, and it cost a product run to find out.
    //
    // Lightrec instruments at BLOCK granularity: arming one PC makes the whole basic block containing it
    // report every store it executes, not just the armed one. So an armed block legitimately contains
    // stores at other PCs, and the assertion fired on the first of them.
    //
    // MEASURED 2026-09-27 on Spyro 1. `PSXPORT_STORE_OBSERVE=80051FF8,8005205C,8005210C,800523E8` drove the
    // product to
    //   [executor:error] frame-update required a completed guest call, but execution exited as fault at
    //   0x8005207C
    //   [watchdog] FAULT (signal): signal = 06
    // 0x8005207C is `lb $v1, 0x52($at)` — the far-moby path entry, three instructions past the armed
    // append at 0x8005205C and in the same basic block. THE PRODUCT DIED FOR BEING WATCHED.
    //
    // An unrequested store in an armed block is counted and reported, never fatal. It also happens to be
    // evidence: execution was inside `func_80051FEC` when this fired, so the moby-list filler RUNS on the
    // product, which the previous data-address arming had reported as never running.
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

  // CLASSIFY THE PC A BUDGET EXIT REPORTS, because that pc is where guest execution resumes and
  // nothing else on the path checks it. Returns the reason unchanged so the call site reads as the
  // ordinary bounded exit it is; the classification is the side effect.
  static ExecutionExitReason recordBudgetExit(Impl &impl, std::uint32_t resumePc) {
    ++impl.counters.budgetExits;
    if (impl.core.currentImageIdentity(resumePc).has_value()) {
      ++impl.counters.budgetExitPcInCodeImage;
    } else {
      ++impl.counters.budgetExitPcOutsideCodeImage;
      // Reported IMMEDIATELY and ALWAYS, not on a stride: a budget exit that hands back an
      // unresolvable pc resumes guest execution somewhere that is not code, and the next thing
      // that happens is a bare fault address with no register holding it and nothing to connect
      // it to this decision. The name and the count are what make it connectable.
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
    counters.executedInstructions = stats.executed_instructions + stats.fallback_instructions;
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

  void reportFallbackTelemetry(std::string_view phase, lucent::Level level) const {
    lucent::log(level,
                "executor",
                lucent::format("Lightrec fallback telemetry [{}]: executor_calls={} executed_blocks={} "
                               "executed_instructions={} memory_callbacks={} device_clock_commits={} "
                               "device_clock_commit_instructions={} fallback_blocks={} "
                               "fallback_instructions={} "
                               "reasons{{compilation_failed={},self_modifying_code={},unsupported_block={},"
                               "load_delay_hazard={},unsafe_instruction_fetch={}}} refused_fallback_blocks={} "
                               "refused_reasons{{compilation_failed={},self_modifying_code={},unsupported_block={},"
                               "load_delay_hazard={},unsafe_instruction_fetch={}}} "
                               "max_fallback_blocks_per_execution={}",
                               phase,
                               counters.calls,
                               counters.executedBlocks,
                               counters.executedInstructions,
                               counters.memoryCallbacks,
                               counters.deviceClockCommits,
                               counters.deviceClockCommitInstructions,
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
  SegmentClockLedger segmentClock;
  StoreObserverCallback storeCallback = nullptr;
  void *storeContext = nullptr;
  StoreObserverReport storeReport{};
  std::unique_ptr<FunctionReach> reach;
};

LightrecExecutor::LightrecExecutor(Core &core, FallbackPolicyProvider fallbackPolicyProvider)
    : impl_(std::make_unique<Impl>(core, fallbackPolicyProvider ? fallbackPolicyProvider : defaultFallbackPolicy)) {}

LightrecExecutor::~LightrecExecutor() {
  reportFallbackTelemetry("shutdown");
  // The store observer's report belongs HERE, beside the executor's own shutdown telemetry, and not
  // on one caller's exit path. Measured 2026-09-27 on Spyro 1: `PSXPORT_STORE_OBSERVE=...` printed
  // `watching 3 guest address(es) for stores` and then nothing at all, through two different drivers
  // and a clean `exit 0`. The reason was placement, not the instrument — the report was called from
  // exactly one place in `native_boot`, immediately after that function's frame loop, and this title
  // does not leave through it: its run printed `Lightrec fallback telemetry [shutdown]`, which comes
  // from THIS destructor, while `frame loop done` from the report's own function never appeared.
  //
  // An armed observer that reports nothing is worse than an unarmed one, because the log already said
  // it was watching: the silence reads as "matched none of the stores" when the truth is "never
  // looked". So the report is emitted from the teardown that every exit path reaches, and the
  // one-call-site version is deleted rather than left to double-report on the paths that did take it.
  store_observe_report(impl_->core);
}

void LightrecExecutor::attachFunctionReach(std::unique_ptr<FunctionReach> reach) {
  impl_->reach = std::move(reach);
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
  // TRANSLATED CODE IS ON THE STACK FROM HERE. Every device access, host dispatch, syscall and pending
  // service in this body runs with a generated Lightrec frame below it, so the override differential's
  // journal must not raise its per-call bound fault out of any of them — see
  // `SideEffectJournal::TranslatedExecutionScope`. `execute`, `executeUntilExit` and `executeFunction`
  // all reach this point, so one mark covers every path translated code runs on.
  const SideEffectJournal::TranslatedExecutionScope translated(impl.core);
  std::uint64_t consumedCycles = 0;
  std::uint64_t hostDispatches = 0;
  std::uint32_t nextPc = guestAddress;
  while (consumedCycles < budget.cycles) {
    boundary.reason = LightrecExecutor::Impl::BoundaryReason::None;
    boundary.pc = 0;
    impl.copyCoreToLightrec();
    lightrec_reset_cycle_count(impl.state, 0);
    // The ledger's baseline and Lightrec's counter are two views of one sequence: both are zeroed
    // here, in the same breath, or the first device access of the next segment would measure its
    // delta against the previous segment's total.
    impl.segmentClock.beginSegment();
    const lightrec_execution_stats before = *lightrec_get_execution_stats(impl.state);
    // A segment ends at the host field clock's deadline as well as at the caller's budget: the clock
    // raises its request from instruction accounting, which only runs between segments, so a guest
    // loop waiting on the field's work would otherwise spin through the whole budget in one segment.
    std::uint64_t segmentCycleBudget = budget.cycles - consumedCycles;
    if (impl.core.game) {
      const std::uint64_t untilFieldDue = hostTurnTicksUntilDue(impl.core);
      if (untilFieldDue != 0) {
        segmentCycleBudget = std::min(segmentCycleBudget, untilFieldDue);
      }
      // And at the next device deadline. Device time is folded into the clock on a device register
      // access or at a segment end, so a guest spinning on plain RAM (an STR player's bounded pop of a
      // ring only the CD interrupt fills) would otherwise run the whole budget before its interrupt
      // exists. Zero means the deadline is already due: the accounting below folds it, no cap needed.
      const std::optional<std::uint64_t> untilDeviceEvent = impl.core.game->timing.ticksUntilDeviceEvent();
      if (untilDeviceEvent && *untilDeviceEvent != 0) {
        segmentCycleBudget = std::min(segmentCycleBudget, *untilDeviceEvent);
      }
    }
    // THE BLOCK START IS CAPTURED BEFORE THE CALL, because `lightrec_execute` overwrites `nextPc`
    // with the block's EXIT pc. Without this the executor cannot say which instructions produced a
    // wild target, only that it produced one - and on MMX4 that is the whole question. It is a
    // segment-level quantity, exactly like the exit pc, so it carries the same block-boundary
    // accounting that makes the live register dump trustworthy.
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
      // The segment's own accounting is still the EXACT instruction count, less whatever the
      // device-access commits already put on the clock. So a consumer that only ever looks at the
      // clock between segments — the CDC drive clock, the display fields, the host turn,
      // `rootCounter2OriginTicks` — sees exactly the values it saw before this charge existed, and
      // the only thing that changes is that a guest reading a device register mid-segment now sees
      // progress instead of a frozen value.
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
    if (auto requested = impl.core.executionControl().consume()) {
      requested->cycles += consumedCycles;
      // A request that did not state a resume address gets the standing architectural PC. One that
      // DID state one keeps it: overwriting it unconditionally discarded the requester's deliberate
      // continuation. See the contract in execution_control.h.
      if (requested->guestPc == 0u) {
        requested->guestPc = impl.core.pc;
      }
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
        // THE BOUNDARY'S BLOCK START AND EXIT TARGET ARE CAPTURED HERE, AND REPORTED ONLY IF THE
        // DISPATCH ACTUALLY FAULTS.
        //
        // The first version of this diagnostic was written the other way round: it fired on a
        // PREDICATE (`currentImageIdentity` finds no image) instead of on the OUTCOME, and it
        // stayed silent on Mega Man X4's fatal fault at 0x0113D7D0 while reporting thousands of
        // ordinary 0x0/0xA0/0xB0 transfers. Two sites in this same file consult the same identity
        // lookup and it did not agree between them, so a predicate built on it cannot be trusted to
        // fire. Keying on the outcome - the dispatch returned a Fault - cannot miss, and it is the
        // event that actually matters. It also cannot spam: a healthy run does not fault here.
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
        if (auto requested = impl.core.executionControl().consume()) {
          requested->cycles += consumedCycles;
          if (requested->guestPc == 0u) {
            requested->guestPc = impl.core.pc;
          }
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
        if (requested->guestPc == 0u) {
          requested->guestPc = impl.core.pc;
        }
        return *requested;
      }
      if (dispatchHostServices) {
        nextPc = impl.core.pc;
        continue;
      }
      return {ExecutionExitReason::HostService, nextPc, consumedCycles, "pending work"};
    }
    if (consumedCycles < budget.cycles) {
      // The segment ended at the host field clock's deadline, not at the caller's budget. The
      // accounting above raised the owed turn; the next block boundary takes it.
      continue;
    }
    return {Impl::recordBudgetExit(impl, nextPc), nextPc, consumedCycles, "cycle budget exhausted"};
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
