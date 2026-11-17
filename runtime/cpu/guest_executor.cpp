#include "guest_executor.h"

#include "core.h"
#include "execution_control.h"
#include "execution_ledger.h"
#include "execution_services.h"
#include "executor_state.h"
#include "executor_trace.h"
#include "function_reach.h"
#include "game.h"
#include "guest_call_site.h"
#include "guest_interpreter.h"
#include "guest_memory_map.h"
#include "native_dispatch.h"
#include "recompiler.h"
#include "side_effect_journal.h"
#include "store_observe.h"
#include "watchdog.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace psx::cpu {

struct GuestExecutor::Impl {
  explicit Impl(Core &owner) : core(owner), state(owner), recompiler(recompiler::Recompiler::create(state)) {
    if (recompiler != nullptr) {
      recompiler->setBlockEntry(
          [](void *impl, std::uint32_t pc, RememberedVerdict *verdict) {
            return static_cast<Impl *>(impl)->enterBlock(pc, verdict);
          },
          this);
    }
    psx::platform::Watchdog::watchGuest(&enteredPc, core.r);
  }

  ~Impl() {
    psx::platform::Watchdog::forgetGuest(&enteredPc);
  }

  enum class SegmentExit : std::uint8_t {
    Budget,
    Boundary,
    CheckInterrupt,
    Syscall,
    Break,
    Fault,
  };

  struct Segment {
    SegmentExit exit = SegmentExit::Budget;
    std::uint32_t pc = 0;
    const char *fault = nullptr;
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
  };

  // ---- Block entries ---------------------------------------------------------------------------

  // The block entry at `guestPc`: true when the segment stops here for the boundary's reason. `verdict`, when the
  // caller keeps one for this entry (a translated block entered through a link), answers native dispatch while it
  // holds.
  bool enterBlock(std::uint32_t guestPc, RememberedVerdict *verdict = nullptr) {
    ++state.counters.executedBlocks;
    enteredPc = guestPc;
    if (trace) {
      trace->enter(core, state, guestPc, state.counters.executedBlocks);
    }
    latchWriter(guestPc);
    if (reach) {
      reach->observe(guestPc);
    }
    BoundaryContext &boundary = *running;
    if (boundary.returnAddress && guestPc == *boundary.returnAddress) {
      boundary.reason = BoundaryReason::GuestReturn;
    } else if (boundary.skipPendingBoundaryOnce) {
      boundary.skipPendingBoundaryOnce = false;
    } else if (pendingWorkDeliverable()) {
      boundary.reason = BoundaryReason::PendingWork;
    }
    if (boundary.reason == BoundaryReason::None && boundary.dispatchHostServices &&
        (verdict != nullptr ? core.nativeDispatcher().classify(guestPc, *verdict)
                            : core.nativeDispatcher().classify(guestPc)) != GuestHostDispatchKind::ExecuteGuest) {
      boundary.reason = BoundaryReason::HostDispatch;
    }
    if (boundary.reason == BoundaryReason::None) {
      return false;
    }
    boundary.pc = guestPc;
    return true;
  }

  // Every block entry passes here, so a store is attributed to its block and enclosing call.
  void latchWriter(std::uint32_t guestPc) {
    const std::uint32_t ra = core.r[kRa];
    core.writerLatch.enterBlock(guestPc, ra, core.r, [this, ra] {
      return callWordBefore(ra);
    });
  }

  // The word at ra - 8 when it is in main RAM (read as Core::mem_r32 reads it), else 0.
  std::uint32_t callWordBefore(std::uint32_t ra) const {
    const std::uint32_t site = ra - kJalReturnOffset;
    const std::optional<std::uint32_t> offset = mainRamOffset(site);
    if (!offset) {
      return 0u;
    }
    if (*offset > kMainRamBytes - 4u) {
      return core.mem_r32(site); // straddles the end of RAM: mem_r32's own answer
    }
    std::uint32_t word = 0;
    std::memcpy(&word, &core.ram[*offset], sizeof(word));
    return word;
  }

  // ---- Segments --------------------------------------------------------------------------------

  // Runs from the block entry at `pc` until a block entry finds the segment budget spent, a stop
  // requested or a boundary reason, or an instruction leaves the guest engine (syscall, break, fault).
  Segment runSegment(std::uint32_t pc, std::uint64_t budget) {
    state.stopRequested.store(false, std::memory_order_relaxed);
    state.checkInterrupt = false;
    const SegmentLimits limits{state.executed, budget};
    auto leave = [&](SegmentExit exit, std::uint32_t at, const char *fault = nullptr) {
      state.settleLoads();
      return Segment{exit, at, fault};
    };
    // A stop lands every load still in flight (leave), so host code at a dispatch reads its arguments from Core.
    if (enterBlock(pc)) {
      return leave(SegmentExit::Boundary, pc);
    }
    for (;;) {
      const BlockOutcome block =
          recompiler != nullptr ? recompiler->runBlock(pc, limits) : interpretBlock(state, pc, pc, limits);
      switch (block.end) {
      case BlockEnd::Syscall:
        return leave(SegmentExit::Syscall, block.pc);
      case BlockEnd::Break:
        return leave(SegmentExit::Break, block.pc);
      case BlockEnd::Fault:
        return leave(SegmentExit::Fault, block.pc, block.fault);
      case BlockEnd::Entry:
        break;
      }
      // A block entry at `pc`.
      pc = block.pc;
      if (block.entered) {
        return leave(SegmentExit::Boundary, pc); // a linked entry ran it and met a boundary
      }
      if (state.checkInterrupt || state.stopRequested.load(std::memory_order_relaxed)) {
        return leave(SegmentExit::CheckInterrupt, pc);
      }
      if (state.executed - limits.start >= budget) {
        return leave(SegmentExit::Budget, pc);
      }
      if (enterBlock(pc)) {
        return leave(SegmentExit::Boundary, pc);
      }
      if (block.next != pc + 4u) {
        // The entry is a jump target reached in a delay slot (a branch in a branch's delay slot).
        ++state.counters.faults;
        return leave(SegmentExit::Fault, pc, "a branch in a delay slot is unsupported");
      }
    }
  }

  [[nodiscard]] bool pendingWorkDeliverable() const {
    // Pending work first: it is almost always clear, so the common entry reads one word.
    return __atomic_load_n(&core.pending_work, __ATOMIC_RELAXED) != 0 && core.game && core.active_native_address == 0 &&
           core.pending_guest_redirect == 0;
  }

  // Pending work served where the guest stopped; a result when the caller, not this executor, resumes the guest.
  std::optional<ExecutionResult> servicePendingWorkAt(BoundaryContext &boundary, std::uint64_t consumedCycles) {
    servicePendingWork(core);
    if (auto requested = core.executionControl().consume(core.pc, consumedCycles)) {
      return requested;
    }
    if (!boundary.dispatchHostServices) {
      return ExecutionResult{ExecutionExitReason::HostService, core.pc, consumedCycles, "pending work"};
    }
    boundary.skipPendingBoundaryOnce = __atomic_load_n(&core.pending_work, __ATOMIC_RELAXED) != 0;
    return std::nullopt;
  }

  // Classifies the resume pc a budget exit hands back; an unresolvable one is reported at once.
  ExecutionExitReason recordBudgetExit(std::uint32_t resumePc) {
    ++state.counters.budgetExits;
    if (core.imageCatalog().resolve(resumePc).has_value()) {
      ++state.counters.budgetExitPcInCodeImage;
    } else {
      ++state.counters.budgetExitPcOutsideCodeImage;
      lucent::error("executor",
                    "budget exit reported resume pc 0x{:08X}, which is NOT in any loaded code "
                    "image; guest execution will resume there. {} of {} budget exit(s) so far "
                    "reported a pc outside every code image",
                    resumePc,
                    state.counters.budgetExitPcOutsideCodeImage,
                    state.counters.budgetExits);
    }
    return ExecutionExitReason::BudgetExhausted;
  }

  Core &core;
  ExecutorState state;
  // The engine guest blocks run on wherever the host has a backend; null means the interpreter runs every block.
  std::unique_ptr<recompiler::Recompiler> recompiler;
  // The block last entered, for the watchdog's report.
  volatile std::uint32_t enteredPc = 0;
  BoundaryContext *running = nullptr;
  std::unique_ptr<FunctionReach> reach;
  std::unique_ptr<ExecutorTrace> trace;
};

GuestExecutor::GuestExecutor(Core &core) : impl_(std::make_unique<Impl>(core)) {}

GuestExecutor::~GuestExecutor() {
  logLedger(impl_->state.counters, "shutdown", lucent::Level::Info);
  psx::cpu::reportStoreObserver(impl_->state.storeReport);
}

void GuestExecutor::interpretOnly() {
  impl_->recompiler.reset();
  lucent::info("executor", "every guest block runs on the interpreter (PSXPORT_EXEC_INTERPRET)");
}

void GuestExecutor::noLinking() {
  if (impl_->recompiler != nullptr) {
    impl_->recompiler->disableLinking();
    lucent::info("executor", "translated blocks stay unlinked (PSXPORT_EXEC_NOLINK)");
  }
}

void GuestExecutor::attachTrace(std::unique_ptr<ExecutorTrace> trace) {
  impl_->trace = std::move(trace);
}

void GuestExecutor::attachFunctionReach(std::unique_ptr<FunctionReach> reach) {
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

ExecutionResult GuestExecutor::executeWithBoundary(std::uint32_t guestAddress,
                                                   std::optional<std::uint32_t> returnAddress,
                                                   bool dispatchHostServices,
                                                   ExecutionBudget budget) {
  Impl &impl = *impl_;
  ++impl.state.counters.calls;
  Impl::BoundaryContext boundary{.returnAddress = returnAddress, .dispatchHostServices = dispatchHostServices};
  std::uint64_t consumedCycles = 0;
  std::uint64_t hostDispatches = 0;
  std::uint32_t nextPc = guestAddress;
  while (consumedCycles < budget.cycles) {
    boundary.reason = Impl::BoundaryReason::None;
    boundary.pc = 0;
    impl.core.gte().bind();
    const std::uint64_t before = impl.state.executed;
    impl.state.segmentClock.beginSegment(before);
    // A segment also ends at the host field deadline and the next device deadline, which are only
    // folded into the clock between segments.
    // At most 32 bits of instructions, so every commit of the segment clock fits its delta.
    std::uint64_t segmentCycleBudget =
        std::min<std::uint64_t>(budget.cycles - consumedCycles, std::numeric_limits<std::uint32_t>::max());
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
    // The start names the block behind a wild target.
    const std::uint32_t blockStart = nextPc;
    Impl::BoundaryContext *const enclosing = std::exchange(impl.running, &boundary);
    Impl::Segment segment;
    {
      // Guest frames are on the stack only in here; the journal must not raise through them.
      const SideEffectJournal::GuestExecutionScope guest(impl.core);
      segment = impl.runSegment(nextPc, segmentCycleBudget);
    }
    impl.running = enclosing;
    nextPc = segment.pc;
    impl.core.pc = nextPc;
    impl.core.r[0] = 0;
    if (impl.core.sideEffectJournal != nullptr) {
      impl.core.sideEffectJournal->stopDeferredDiscardedRun();
    }
    const std::uint64_t segmentInstructions = impl.state.executed - before;
    consumedCycles += segmentInstructions;
    impl.state.counters.executedInstructions = impl.state.executed;
    if (impl.state.storeReport.armed) {
      impl.state.storeReport.executedInstructions += segmentInstructions;
    }
    if (impl.core.game) {
      // The exact instruction count, less what device-access commits already charged.
      accountExecutedInstructions(impl.core, impl.state.segmentClock.uncommitted(segmentInstructions));
      impl.core.game->display.showIfDue();
    }

    if (auto requested = impl.core.executionControl().consume(impl.core.pc, consumedCycles)) {
      return *requested;
    }
    switch (segment.exit) {
    case Impl::SegmentExit::Fault:
      lucent::error("executor", "guest fault at 0x{:08X}: {}", nextPc, segment.fault);
      return {ExecutionExitReason::Fault, nextPc, consumedCycles, segment.fault};
    case Impl::SegmentExit::Syscall: {
      const auto syscall = handleSyscall(impl.core, nextPc);
      if (syscall != SyscallResult::Handled) {
        ++impl.state.counters.faults;
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
    case Impl::SegmentExit::Break: {
      const std::uint32_t instruction = impl.core.mem_r32(nextPc);
      handleBreak(impl.core, (instruction >> 6u) & 0xfffffu);
      impl.core.pc = nextPc + 4u;
      return {ExecutionExitReason::HostService, impl.core.pc, consumedCycles, "break"};
    }
    case Impl::SegmentExit::Boundary:
      switch (boundary.reason) {
      case Impl::BoundaryReason::GuestReturn:
        return {ExecutionExitReason::GuestReturn, boundary.pc, consumedCycles};
      case Impl::BoundaryReason::HostDispatch: {
        if (hostDispatches >= budget.maxHostDispatches) {
          return {impl.recordBudgetExit(boundary.pc), boundary.pc, consumedCycles, "host dispatch budget exhausted"};
        }
        ++hostDispatches;
        ++impl.state.counters.hostDispatches;
        ExecutionResult result = dispatchGuestHostService(impl.core, boundary.pc);
        result.cycles += consumedCycles;
        if (!result.returned()) {
          if (result.reason == ExecutionExitReason::Fault) {
            lucent::error("executor",
                          "host dispatch to 0x{:08X} FAILED ({}); the segment that produced this target began at "
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
      case Impl::BoundaryReason::PendingWork:
        if (auto handedBack = impl.servicePendingWorkAt(boundary, consumedCycles)) {
          return *handedBack;
        }
        nextPc = impl.core.pc;
        continue;
      case Impl::BoundaryReason::None:
        ++impl.state.counters.faults;
        return {ExecutionExitReason::Fault, nextPc, consumedCycles, "unclassified block boundary stop"};
      }
      break;
    case Impl::SegmentExit::CheckInterrupt:
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
    case Impl::SegmentExit::Budget:
      break;
    }
    if (consumedCycles < budget.cycles) {
      // Ended at the field or device deadline, not the caller's budget: what the deadline raised is taken before the
      // guest resumes, since a loop inside one block reaches no block entry until the next deadline.
      if (impl.pendingWorkDeliverable()) {
        if (auto handedBack = impl.servicePendingWorkAt(boundary, consumedCycles)) {
          return *handedBack;
        }
        nextPc = impl.core.pc;
      }
      continue;
    }
    return {impl.recordBudgetExit(nextPc), nextPc, consumedCycles, "cycle budget exhausted"};
  }
  return {ExecutionExitReason::BudgetExhausted, nextPc, consumedCycles, "cycle budget exhausted"};
}

ExecutionResult GuestExecutor::executeUntilExit(std::uint32_t guestAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, std::nullopt, true, budget);
}

ExecutionResult
GuestExecutor::executeFunction(std::uint32_t guestAddress, std::uint32_t returnAddress, ExecutionBudget budget) {
  return executeWithBoundary(guestAddress, returnAddress, true, budget);
}

void GuestExecutor::requestStop() {
  impl_->state.stopRequested.store(true, std::memory_order_relaxed);
}

void GuestExecutor::invalidate(GuestAddressRange range, ExecutableWriteSource source) {
  ++impl_->state.counters.invalidations;
  ++impl_->state.counters.invalidationsBySource[static_cast<std::size_t>(source)];
  if (impl_->recompiler != nullptr) {
    impl_->recompiler->invalidate(range);
  }
}

void GuestExecutor::invalidateAll(ExecutableWriteSource source) {
  ++impl_->state.counters.invalidations;
  ++impl_->state.counters.invalidationsBySource[static_cast<std::size_t>(source)];
  if (impl_->recompiler != nullptr) {
    impl_->recompiler->invalidateAll();
  }
}

StoreObserverStatus GuestExecutor::configureStoreObserver(std::span<const std::uint32_t> targets,
                                                          StoreObserverCallback callback,
                                                          void *context) {
  Impl &impl = *impl_;
  if ((targets.empty() && (callback || context)) || targets.size() > kMaxObservedStoreTargets ||
      (!targets.empty() && callback == nullptr)) {
    return StoreObserverStatus::InvalidConfiguration;
  }
  if (targets.empty()) {
    impl.state.storeReport.armed = false;
    impl.state.storeCallback = nullptr;
    impl.state.storeContext = nullptr;
    return StoreObserverStatus::Configured;
  }
  impl.state.storeReport = {};
  impl.state.storeReport.targetCount = targets.size();
  impl.state.storeReport.armed = true;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    impl.state.storeReport.targets[i].guestPc = targets[i];
  }
  impl.state.storeCallback = callback;
  impl.state.storeContext = context;
  return StoreObserverStatus::Configured;
}

StoreObserverReport GuestExecutor::storeObserverReport() const {
  return impl_->state.storeReport;
}

const ExecutorCounters &GuestExecutor::counters() const {
  return impl_->state.counters;
}

} // namespace psx::cpu
