#include "config.h"
#include "coro.h"
#include "dynarec_capabilities.h"
#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"

#include "testutil.h"

#include <lucent/log.h>

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
};

std::unique_ptr<Game> makeGame(Runtime &runtime) {
  psxport_install_game(runtime);
  return std::make_unique<Game>();
}

constexpr std::uint32_t encodeJal(std::uint32_t target) {
  return 0x0c000000u | ((target >> 2u) & 0x03ffffffu);
}

constexpr std::uint32_t encodeJ(std::uint32_t target) {
  return 0x08000000u | ((target >> 2u) & 0x03ffffffu);
}

constexpr std::uint32_t kCaller = 0x00010000u;
constexpr std::uint32_t kCallee = 0x00010100u;
constexpr std::uint32_t kInnerCallee = 0x00010140u;
constexpr std::uint32_t kNestedReturn = 0x00010180u;
constexpr std::uint32_t kWriter = 0x00010200u;
constexpr std::uint32_t kMainFallback = 0x00010300u;
constexpr std::uint32_t kOuterReturn = 0x00010f00u;
constexpr std::uint32_t kObservedWriter = 0x80010500u;
constexpr std::uint32_t kMainRegisterValue = 0x13579bdfu;
constexpr std::uint32_t kTaskRegisterValue = 0x2468ace0u;

psx::cpu::ImageIdentity installTestImage(Core &core) {
  return core.imageCatalog().activate("dynarec-contract", {kCaller, kOuterReturn + 12u}, 0x44594e41524543ull);
}

void writeReturningCaller(Core &core) {
  core.mem_w32(kCaller, 0x03e08021u); // addu s0, ra, zero
  core.mem_w32(kCaller + 4u, encodeJal(kCallee));
  core.mem_w32(kCaller + 8u, 0u);           // delay-slot nop
  core.mem_w32(kCaller + 12u, 0x24420002u); // addiu v0, v0, 2
  core.mem_w32(kCaller + 16u, 0x02000008u); // jr s0
  core.mem_w32(kCaller + 20u, 0u);          // delay-slot nop

  core.mem_w32(kOuterReturn, 0x24177badu);      // must not execute: addiu s7, zero, 0x7bad
  core.mem_w32(kOuterReturn + 4u, 0x1000ffffu); // stable self-loop if the boundary is missed
  core.mem_w32(kOuterReturn + 8u, 0u);
}

int nativeOverrideCalls = 0;
std::uint32_t nativeOverrideActiveAddress = 0;

void nativeCallee(Core *core) {
  ++nativeOverrideCalls;
  nativeOverrideActiveAddress = core->active_native_address;
  core->r[2] = 40u;
}

void nativeFrameExit(Core *core) {
  core->r[17] = 7u;
  psx::cpu::requestExecutionExit(*core, psx::cpu::ExecutionExitReason::FrameBoundary);
}

int callOriginalOverrideCalls = 0;
std::uint32_t callOriginalActiveAddress = 0;
std::uint32_t callOriginalActiveAddressAfter = 0;
std::uint32_t callOriginalPcAfter = 0;
int callOriginalPendingWorkAfter = 0;
psx::cpu::NativeKey callOriginalKey{};
psx::cpu::ExecutionResult callOriginalResult{};

void nativeCalleeCallingOriginal(Core *core) {
  ++callOriginalOverrideCalls;
  callOriginalActiveAddress = core->active_native_address;
  core->pending_work = Core::PW_HOST;
  callOriginalResult = psx::cpu::callOriginal(*core, callOriginalKey, psx::cpu::ExecutionBudget::fromCycles(100));
  callOriginalActiveAddressAfter = core->active_native_address;
  callOriginalPcAfter = core->pc;
  callOriginalPendingWorkAfter = core->pending_work;
  if (callOriginalResult.returned()) {
    core->r[2] += 9u;
  }
}

void nativeCallerRunningOriginalUntilExit(Core *core) {
  callOriginalResult = psx::cpu::callOriginalUntilExit(*core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  callOriginalPcAfter = core->pc;
  callOriginalActiveAddressAfter = core->active_native_address;
  psx::cpu::requestExecutionExit(*core, callOriginalResult);
}

int outerNativeCalls = 0;
int innerNativeCalls = 0;
std::uint32_t outerActiveAddressBeforeNested = 0;
std::uint32_t outerActiveAddressAfterNested = 0;
std::uint32_t innerActiveAddress = 0;
std::uint32_t outerPcAfterNested = 0;
psx::cpu::ExecutionResult nestedNativeResult{};
std::unique_ptr<Coro> parkedFiber;
R3000 parkedTaskRegisters{};
std::uint32_t parkedTaskNativeAddress = 0;
psx::cpu::ExecutionResult parkedTaskResult{};
bool parkedTaskResumedIntact = false;
bool exerciseCrossThreadFallback = false;
psx::cpu::ExecutionResult mainFallbackResult{};

std::vector<std::pair<lucent::Level, std::string>> telemetryLines;

struct StoreTrace {
  Core *core = nullptr;
  psx::cpu::LightrecExecutor *executor = nullptr;
  std::uint32_t pc[2]{};
  std::uint32_t value[2]{};
  std::uint32_t source[2]{};
  std::uint32_t cycle[2]{};
  psx::cpu::StoreObservationPhase phase[2]{};
  std::size_t calls = 0;
  std::size_t sentinelCalls = 0;
  psx::cpu::StoreObserverStatus reentrantDisarm = psx::cpu::StoreObserverStatus::Configured;
};

void captureStore(const psx::cpu::StoreObservation &observation, void *data) noexcept {
  auto &trace = *static_cast<StoreTrace *>(data);
  if (observation.guestPc == 0xfffffffcu) {
    ++trace.sentinelCalls;
  }
  const auto index = trace.calls++;
  if (index == 0) {
    trace.reentrantDisarm = trace.executor->configureStoreObserver({}, nullptr, nullptr);
  }
  if (index >= 2) {
    return;
  }
  trace.pc[index] = observation.guestPc;
  trace.value[index] = trace.core->mem_r32(0x40u);
  trace.source[index] = observation.gpr[9];
  trace.cycle[index] = observation.guestCycle;
  trace.phase[index] = observation.phase;
}

class TelemetryCapture final {
public:
  TelemetryCapture() {
    telemetryLines.clear();
    lucent::set_sink([](lucent::Level level, std::string_view line) {
      telemetryLines.emplace_back(level, std::string(line));
    });
  }
  ~TelemetryCapture() {
    lucent::set_sink(nullptr);
  }
};

bool telemetryContains(lucent::Level level, std::string_view needle) {
  for (const auto &[actualLevel, line] : telemetryLines) {
    if (actualLevel == level && line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void innerNativeCallee(Core *core) {
  ++innerNativeCalls;
  innerActiveAddress = core->active_native_address;
  core->r[2] = 40u;
}

void outerNativeCallee(Core *core) {
  ++outerNativeCalls;
  outerActiveAddressBeforeNested = core->active_native_address;
  const std::uint32_t savedReturn = core->r[31];
  core->r[31] = kNestedReturn;
  nestedNativeResult = psx::cpu::dispatchGuest(*core, kInnerCallee, psx::cpu::ExecutionBudget::fromCycles(20));
  outerActiveAddressAfterNested = core->active_native_address;
  outerPcAfterNested = core->pc;
  core->r[31] = savedReturn;
}

void nativeParkTask(Core *core) {
  core->r[18] = kTaskRegisterValue;
  parkedFiber->yield();
  parkedTaskResumedIntact = core->r[18] == kTaskRegisterValue && core->active_native_address == kWriter;
  core->r[2] = 40u;
}

void nativeLaunchParkedTask(Core *core) {
  const R3000 mainRegisters = *static_cast<R3000 *>(core);
  const std::uint32_t mainNativeAddress = core->active_native_address;
  core->r[29] = 0x001ff000u;
  core->r[31] = kNestedReturn;
  core->active_native_address = 0;
  parkedFiber = std::make_unique<Coro>();
  parkedFiber->start([core] {
    parkedTaskResult = psx::cpu::dispatchGuest(*core, kInnerCallee, psx::cpu::ExecutionBudget::fromCycles(100));
  });
  parkedFiber->resume();
  parkedTaskRegisters = *static_cast<R3000 *>(core);
  parkedTaskNativeAddress = core->active_native_address;
  *static_cast<R3000 *>(core) = mainRegisters;
  core->active_native_address = mainNativeAddress;
  if (exerciseCrossThreadFallback) {
    mainFallbackResult = core->lightrecExecutor().execute(kMainFallback, psx::cpu::ExecutionBudget::fromCycles(20));
    *static_cast<R3000 *>(core) = mainRegisters;
    core->active_native_address = mainNativeAddress;
  }
}

void installParkedTaskFixture(Core &core, const psx::cpu::ImageIdentity &image, bool fallbackAfterResume) {
  writeReturningCaller(core);
  core.mem_w32(kInnerCallee, 0x03e08821u);             // addu s1, ra, zero
  core.mem_w32(kInnerCallee + 4u, encodeJal(kWriter)); // task parks in native callback
  core.mem_w32(kInnerCallee + 8u, 0u);
  if (fallbackAfterResume) {
    core.mem_w32(kInnerCallee + 12u, 0x10000001u); // branch in delay slot requires fallback
    core.mem_w32(kInnerCallee + 16u, encodeJ(kInnerCallee + 24u));
    core.mem_w32(kInnerCallee + 20u, 0u);
    core.mem_w32(kInnerCallee + 24u, 0x02200008u); // jr s1
    core.mem_w32(kInnerCallee + 28u, 0u);
    core.mem_w32(kMainFallback, 0x10000001u);
    core.mem_w32(kMainFallback + 4u, encodeJ(kMainFallback + 12u));
    core.mem_w32(kMainFallback + 8u, 0u);
    core.mem_w32(kMainFallback + 12u, 0x1000ffffu); // stable self-loop
    core.mem_w32(kMainFallback + 16u, 0u);
  } else {
    core.mem_w32(kInnerCallee + 12u, 0x02200008u); // jr s1
    core.mem_w32(kInnerCallee + 16u, 0u);
  }
  core.mem_w32(kNestedReturn, 0x24177badu); // must not execute
  CHECK(core.nativeDispatcher().install({{image, kCallee}, "launch-parked-task", nativeLaunchParkedTask}));
  CHECK(core.nativeDispatcher().install({{image, kWriter}, "park-task", nativeParkTask}));
  exerciseCrossThreadFallback = fallbackAfterResume;
  mainFallbackResult = {};
  parkedTaskResult = {};
  parkedTaskResumedIntact = false;
  core.r[18] = kMainRegisterValue;
  core.r[29] = 0x001fffe0u;
  core.r[31] = kOuterReturn;
}

std::uint32_t resumeParkedTask(Core &core) {
  const R3000 mainRegisters = *static_cast<R3000 *>(&core);
  const std::uint32_t mainNativeAddress = core.active_native_address;
  *static_cast<R3000 *>(&core) = parkedTaskRegisters;
  core.active_native_address = parkedTaskNativeAddress;
  parkedFiber->resume();
  parkedTaskRegisters = *static_cast<R3000 *>(&core);
  const std::uint32_t taskNativeAddress = core.active_native_address;
  *static_cast<R3000 *>(&core) = mainRegisters;
  core.active_native_address = mainNativeAddress;
  return taskNativeAddress;
}

} // namespace

static void test_supported_syscall_resumes_function_and_retains_checkpoint_exit() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x0000000cu);      // syscall
  core.mem_w32(kCaller + 4u, 0x24500005u); // addiu s0, v0, 5
  core.mem_w32(kCaller + 8u, 0x03e00008u); // jr ra
  core.mem_w32(kCaller + 12u, 0u);
  core.r[4] = 1u;
  core.r[31] = kOuterReturn;
  game->hle.irq_enabled = 1;
  const auto checkpoint = core.lightrecExecutor().execute(kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(checkpoint.reason, psx::cpu::ExecutionExitReason::InterruptOrException);
  CHECK_EQ(checkpoint.guestPc, kCaller + 4u);
  CHECK_EQ(core.r[16], 0u);
  CHECK_EQ(core.cop0[14], kCaller);

  game->hle.irq_enabled = 1;
  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(result.guestPc, kOuterReturn);
  CHECK_EQ(core.r[16], 6u);
  CHECK_EQ(game->hle.irq_enabled, 0);
  CHECK_EQ(core.cop0[13] & 0x7cu, 0x20u);
  CHECK_EQ(core.cop0[14], kCaller);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_unsupported_syscall_preserves_state_and_refuses_continuation() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x0000000cu);
  core.mem_w32(kCaller + 4u, 0x24100005u); // must not run: addiu s0, zero, 5
  core.r[4] = 99u;
  core.r[2] = 42u;
  core.r[31] = kOuterReturn;
  core.cop0[12] = 0x00600001u;
  core.cop0[14] = 0x100u;
  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(result.guestPc, kCaller);
  CHECK_EQ(core.r[2], 42u);
  CHECK_EQ(core.r[16], 0u);
  CHECK_EQ(core.cop0[12], 0x00600001u);
  CHECK_EQ(core.cop0[14], 0x100u);
}

static void test_until_exit_routes_native_entry_syscall_pending_work_and_frame_exit() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  nativeOverrideCalls = 0;
  CHECK(core.nativeDispatcher().install({{image, kCallee}, "native-entry", nativeCallee}));
  CHECK(core.nativeDispatcher().install({{image, kInnerCallee}, "native-frame-exit", nativeFrameExit}));
  core.mem_w32(kCaller, 0x0000000cu);      // syscall ExitCriticalSection
  core.mem_w32(kCaller + 4u, 0x24500005u); // addiu s0, v0, 5
  core.mem_w32(kCaller + 8u, encodeJal(kInnerCallee));
  core.mem_w32(kCaller + 12u, 0u);
  core.mem_w32(kCaller + 16u, 0x24177badu); // must not run after requested frame exit
  core.r[4] = 2u;
  core.r[31] = kCaller;
  core.pending_work = Core::PW_HOST;

  const auto result = psx::cpu::dispatchGuestUntilExit(core, kCallee, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::FrameBoundary);
  CHECK_EQ(nativeOverrideCalls, 1);
  CHECK_EQ(core.r[16], 5u);
  CHECK_EQ(core.r[17], 7u);
  CHECK_EQ(core.r[23], 0u);
  CHECK_EQ(core.pending_work & Core::PW_HOST, 0);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_delay_slot_syscall_is_refused_without_sequential_resume() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x10000002u);      // beq zero, zero, caller+12
  core.mem_w32(kCaller + 4u, 0x0000000cu); // syscall in taken branch delay slot
  core.mem_w32(kCaller + 8u, 0x24100005u); // must not run as sequential continuation
  core.mem_w32(kCaller + 12u, 0x24110006u);
  core.r[4] = 1u;
  core.r[31] = kOuterReturn;
  game->hle.irq_enabled = 1;
  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(result.guestPc, kCaller + 4u);
  CHECK_EQ(game->hle.irq_enabled, 1);
  CHECK_EQ(core.r[16], 0u);
  CHECK_EQ(core.r[17], 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_syscall_continuation_remains_bounded() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x0000000cu);
  core.mem_w32(kCaller + 4u, 0x08000000u | (kCaller >> 2u)); // j syscall
  core.mem_w32(kCaller + 8u, 0u);
  core.r[4] = 2u;
  core.r[31] = kOuterReturn;
  const auto result = psx::cpu::dispatchGuestUntilExit(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(20));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::BudgetExhausted);
  CHECK(result.cycles >= 20u);
  CHECK(result.cycles <= 24u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_native_only_self_loop_exhausts_dispatch_budget_without_fabricated_cycles() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  nativeOverrideCalls = 0;
  CHECK(core.nativeDispatcher().install({{image, kCallee}, "native-loop", nativeCallee}));
  core.r[31] = kCallee;
  auto budget = psx::cpu::ExecutionBudget::fromCycles(100);
  budget.maxHostDispatches = 3;
  const auto result = psx::cpu::dispatchGuestUntilExit(core, kCallee, budget);
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::BudgetExhausted);
  CHECK_EQ(result.guestPc, kCallee);
  CHECK_EQ(result.cycles, 0u);
  CHECK_EQ(nativeOverrideCalls, 3);
  CHECK_EQ(core.lightrecExecutor().counters().hostDispatches, 3u);
  CHECK_EQ(core.lightrecExecutor().counters().executedInstructions, 0u);
  budget.maxHostDispatches = 0;
  const auto refused = psx::cpu::dispatchGuestUntilExit(core, kCallee, budget);
  CHECK_EQ(refused.reason, psx::cpu::ExecutionExitReason::BudgetExhausted);
  CHECK_EQ(nativeOverrideCalls, 3);
  CHECK_EQ(refused.cycles, 0u);
}

static void test_explicit_function_continuation_is_independent_of_incoming_ra() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x02200008u); // jr s1
  core.mem_w32(kCaller + 4u, 0u);
  core.r[17] = kOuterReturn;
  core.r[31] = kCallee;
  const auto result =
      core.lightrecExecutor().executeFunction(kCaller, kOuterReturn, psx::cpu::ExecutionBudget::fromCycles(20));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(result.guestPc, kOuterReturn);
}

static void test_original_until_exit_suppresses_only_its_entry_and_restores_on_frame_exit() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  nativeOverrideCalls = 0;
  CHECK(core.nativeDispatcher().install({{image, kCaller}, "suppressed-entry", nativeCallee}));
  CHECK(core.nativeDispatcher().install({{image, kInnerCallee}, "native-frame-exit", nativeFrameExit}));
  core.mem_w32(kCaller, encodeJal(kInnerCallee));
  core.mem_w32(kCaller + 4u, 0u);
  core.r[31] = kCaller;
  const auto result = psx::cpu::callOriginalUntilExit(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::FrameBoundary);
  CHECK_EQ(nativeOverrideCalls, 0);
  CHECK_EQ(core.r[17], 7u);
  CHECK(core.nativeDispatcher().intercepts({image, kCaller}));
  CHECK_EQ(core.active_native_address, 0u);
  CHECK(!core.executionControl().pending());
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_native_store_widths_and_ram_aliases_invalidate_translated_code() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x24020001u);      // addiu v0, zero, 1
  core.mem_w32(kCaller + 4u, 0x03e00008u); // jr ra
  core.mem_w32(kCaller + 8u, 0u);
  core.r[31] = kOuterReturn;
  CHECK(psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100)).returned());
  CHECK_EQ(core.r[2], 1u);
  std::uint32_t value = 1;
  for (const std::uint32_t width : {1u, 2u, 4u}) {
    for (const std::uint32_t segment : {0u, 0x80000000u, 0xa0000000u}) {
      for (const std::uint32_t mirror : {0u, 0x200000u, 0x400000u, 0x600000u}) {
        ++value;
        const auto before = core.lightrecExecutor().counters().translatedBlocks;
        const auto address = segment | mirror | kCaller;
        if (width == 1u) {
          core.mem_w8(address, static_cast<std::uint8_t>(value));
        } else if (width == 2u) {
          core.mem_w16(address, static_cast<std::uint16_t>(value));
        } else {
          core.mem_w32(address, 0x24020000u | value);
        }
        CHECK(psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100)).returned());
        CHECK_EQ(core.r[2], value);
        CHECK(core.lightrecExecutor().counters().translatedBlocks > before);
      }
    }
  }
  const auto before = core.lightrecExecutor().counters().translatedBlocks;
  core.mem_w32(kCaller, 0x24020000u | value); // same bytes must not require new translated code
  core.mem_w32(kWriter, 0xdeadbeefu);         // outside the translated function
  CHECK(psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100)).returned());
  CHECK_EQ(core.r[2], value);
  CHECK_EQ(core.lightrecExecutor().counters().translatedBlocks, before);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_nested_original_resumes_native_return_result_not_scoped_caller_pc() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  nativeOverrideCalls = 0;
  callOriginalResult = {};
  callOriginalPcAfter = 0;
  callOriginalActiveAddressAfter = 0;
  CHECK(
      core.nativeDispatcher().install({{image, kCaller}, "original-until-exit", nativeCallerRunningOriginalUntilExit}));
  CHECK(core.nativeDispatcher().install({{image, kCallee}, "ordinary-native-return", nativeCallee}));
  CHECK(core.nativeDispatcher().install({{image, kInnerCallee}, "frame-exit", nativeFrameExit}));
  core.mem_w32(kCaller, encodeJal(kCallee));
  core.mem_w32(kCaller + 4u, 0u);
  core.mem_w32(kCaller + 8u, 0x26520001u); // addiu s2, s2, 1: translated continuation runs once
  core.mem_w32(kCaller + 12u, encodeJal(kInnerCallee));
  core.mem_w32(kCaller + 16u, 0u);
  core.r[31] = kCaller;
  const auto result = psx::cpu::dispatchGuestUntilExit(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::FrameBoundary);
  CHECK_EQ(callOriginalResult.reason, psx::cpu::ExecutionExitReason::FrameBoundary);
  CHECK_EQ(result.guestPc, kInnerCallee);
  CHECK_EQ(nativeOverrideCalls, 1);
  CHECK_EQ(core.r[18], 1u);
  CHECK_EQ(core.r[17], 7u);
  CHECK_EQ(callOriginalPcAfter, kCaller);
  CHECK_EQ(callOriginalActiveAddressAfter, kCaller);
  CHECK_EQ(core.active_native_address, 0u);
  CHECK(core.nativeDispatcher().intercepts({image, kCaller}));
  CHECK(!core.executionControl().pending());
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_backend_reports_verified_host_properties() {
  constexpr auto capabilities = psx::cpu::kLightrecBackendCapabilities;
  CHECK(capabilities.available);
  CHECK(capabilities.dynarecDefault);
  CHECK(capabilities.boundedInterpreterFallback);
  CHECK(capabilities.fallbackTelemetry);
  CHECK(capabilities.fallbackThresholdEnforcement);
  CHECK(!capabilities.aarch64CodeGeneration);
  CHECK(capabilities.executableMemoryPublication);
  CHECK(capabilities.instructionCacheCoherence);
  CHECK(capabilities.rangeInvalidation);
  CHECK(capabilities.hostAbiTransitions);
  CHECK_EQ(PSXPORT_HAS_LIGHTREC_BACKEND, 1);
  CHECK_EQ(PSXPORT_HAS_BOUNDED_INTERPRETER_FALLBACK, 1);
  CHECK_EQ(PSXPORT_HAS_LIGHTREC_AARCH64, 0);
}

static void test_real_executor_translates_and_runs_guest_instructions() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  core.mem_w32(entry, 0x2402002au);      // addiu v0, zero, 42
  core.mem_w32(entry + 4u, 0x24030007u); // addiu v1, zero, 7
  core.mem_w32(entry + 8u, 0x1000ffffu); // beq zero, zero, self
  core.mem_w32(entry + 12u, 0u);         // delay-slot nop

  auto &executor = core.lightrecExecutor();
  CHECK(executor.available());
  const auto result = executor.execute(entry, psx::cpu::ExecutionBudget::fromCycles(100));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::BudgetExhausted);
  CHECK(result.cycles >= 100u);
  CHECK_EQ(core.r[2], 42u);
  CHECK_EQ(core.r[3], 7u);
  CHECK(executor.counters().translatedBlocks > 0u);
  CHECK(executor.counters().executedBlocks > 0u);
  CHECK_EQ(executor.counters().fallback.calls, 0u);
  CHECK_EQ(executor.counters().fallback.instructions, 0u);
  CHECK(executor.counters().executedInstructions > 0u);
  CHECK_EQ(game->timing.guestInstructionTicks * 2u, result.cycles);

  TelemetryCapture capture;
  executor.reportFallbackTelemetry("negative-test");
  CHECK(telemetryContains(lucent::Level::Info, "Lightrec fallback telemetry [negative-test]"));
  CHECK(telemetryContains(lucent::Level::Info, "executor_calls=1"));
  CHECK(telemetryContains(lucent::Level::Info, "fallback_blocks=0 fallback_instructions=0"));
  CHECK(telemetryContains(lucent::Level::Info,
                          "reasons{compilation_failed=0,self_modifying_code=0,unsupported_block=0,"
                          "load_delay_hazard=0,unsafe_instruction_fetch=0}"));
  CHECK(telemetryContains(lucent::Level::Info, "refused_fallback_blocks=0"));
}

static void test_selected_store_observer_bridges_exact_jit_pc_and_rejects_unsupported_target() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  auto &executor = core.lightrecExecutor();
  installTestImage(core);
  constexpr std::uint32_t selected = kObservedWriter + 8u;
  constexpr std::uint32_t sentinel = 0xfffffffcu;
  constexpr std::uint32_t unsupported = kObservedWriter + 4u;
  const std::uint32_t targets[] = {selected, sentinel};
  core.mem_w32(kObservedWriter, 0x24080040u);      // addiu t0, zero, 0x40
  core.mem_w32(kObservedWriter + 4u, 0x24090007u); // addiu t1, zero, 7
  core.mem_w32(selected, 0xad090000u);             // sw t1, 0(t0)
  core.mem_w32(kObservedWriter + 12u, 0x240a0009u);
  core.mem_w32(kObservedWriter + 16u, 0x03e00008u); // jr ra
  core.mem_w32(kObservedWriter + 20u, 0u);
  core.mem_w32(0x40u, 0u);
  core.r[31] = kOuterReturn;
  const auto plain =
      executor.executeFunction(kObservedWriter, kOuterReturn, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(plain.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(core.mem_r32(0x40u), 7u);
  CHECK_EQ(core.r[10], 9u);
  const auto warmTranslations = executor.counters().translatedBlocks;
  const auto baselineInstructions = executor.counters().executedInstructions;

  core.mem_w32(0x40u, 0u);
  core.r[8] = core.r[9] = core.r[10] = 0u;
  StoreTrace trace{.core = &core, .executor = &executor};
  CHECK_EQ(executor.configureStoreObserver(targets, captureStore, &trace), psx::cpu::StoreObserverStatus::Configured);
  const auto observed =
      executor.executeFunction(kObservedWriter, kOuterReturn, psx::cpu::ExecutionBudget::fromCycles(100));
  const auto report = executor.storeObserverReport();
  CHECK_EQ(observed.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(trace.calls, 2u);
  CHECK_EQ(trace.sentinelCalls, 0u);
  CHECK_EQ(trace.reentrantDisarm, psx::cpu::StoreObserverStatus::Busy);
  CHECK_EQ(trace.pc[0], selected);
  CHECK_EQ(trace.pc[1], selected);
  CHECK_EQ(trace.phase[0], psx::cpu::StoreObservationPhase::Before);
  CHECK_EQ(trace.phase[1], psx::cpu::StoreObservationPhase::After);
  CHECK_EQ(trace.value[0], 0u);
  CHECK_EQ(trace.value[1], 7u);
  CHECK_EQ(trace.source[0], 7u);
  CHECK_EQ(trace.source[1], 7u);
  CHECK(trace.cycle[1] > trace.cycle[0]);
  CHECK_EQ(core.r[10], 9u);
  CHECK_EQ(observed.cycles, plain.cycles);
  CHECK(executor.counters().translatedBlocks > warmTranslations);
  CHECK_EQ(report.targetCount, 2u);
  CHECK(report.armed);
  CHECK_EQ(report.targets[0].guestPc, selected);
  CHECK_EQ(report.targets[0].before, 1u);
  CHECK_EQ(report.targets[0].after, 1u);
  CHECK_EQ(report.targets[1].guestPc, sentinel);
  CHECK_EQ(report.targets[1].before, 0u);
  CHECK_EQ(report.targets[1].after, 0u);
  CHECK(report.executedJitInstructions > 0u);
  CHECK_EQ(report.executedJitInstructions, executor.counters().executedInstructions - baselineInstructions);
  CHECK_EQ(report.fallbackInstructions, 0u);
  std::fprintf(stderr,
               "  selected store: before=%" PRIu64 " after=%" PRIu64 " sentinel=%" PRIu64 "/%" PRIu64
               " JIT instructions fallback=%" PRIu64 "\n",
               report.targets[0].before,
               report.targets[0].after,
               report.targets[1].before + report.targets[1].after,
               report.executedJitInstructions,
               report.fallbackInstructions);

  CHECK_EQ(executor.configureStoreObserver({}, nullptr, nullptr), psx::cpu::StoreObserverStatus::Configured);
  CHECK(!executor.storeObserverReport().armed);
  core.mem_w32(0x40u, 0u);
  core.r[8] = core.r[9] = core.r[10] = 0u;
  const auto disarmed =
      executor.executeFunction(kObservedWriter, kOuterReturn, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(disarmed.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(trace.calls, 2u);
  CHECK_EQ(disarmed.cycles, plain.cycles);
  CHECK_EQ(core.mem_r32(0x40u), 7u);
  CHECK_EQ(core.r[10], 9u);
  CHECK_EQ(executor.storeObserverReport().executedJitInstructions, report.executedJitInstructions);

  core.mem_w32(0x40u, 0u);
  core.r[8] = core.r[9] = core.r[10] = 0u;
  CHECK_EQ(executor.configureStoreObserver(std::span(&unsupported, 1), captureStore, &trace),
           psx::cpu::StoreObserverStatus::Configured);
  const auto instructionsBefore = executor.counters().executedInstructions;
  const auto fallbackBefore = executor.counters().fallback.calls;
  const auto rejected =
      executor.executeFunction(kObservedWriter, kOuterReturn, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(rejected.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK(rejected.detail.find("selected-store observer") != std::string::npos);
  CHECK_EQ(core.mem_r32(0x40u), 0u);
  CHECK_EQ(executor.counters().executedInstructions, instructionsBefore);
  CHECK_EQ(executor.counters().fallback.calls, fallbackBefore);
  CHECK_EQ(trace.calls, 2u);
  const auto rejectedReport = executor.storeObserverReport();
  CHECK(rejectedReport.armed);
  CHECK_EQ(rejectedReport.targetCount, 1u);
  CHECK_EQ(rejectedReport.targets[0].guestPc, unsupported);
  CHECK_EQ(rejectedReport.targets[0].before, 0u);
  CHECK_EQ(rejectedReport.targets[0].after, 0u);
  CHECK_EQ(rejectedReport.executedJitInstructions, 0u);
  CHECK_EQ(rejectedReport.fallbackInstructions, 0u);
}

static void test_translated_call_dispatches_image_scoped_native_and_resumes_caller() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  writeReturningCaller(core);
  core.mem_w32(kCallee, 0x2402001fu);      // original: addiu v0, zero, 31
  core.mem_w32(kCallee + 4u, 0x03e00008u); // jr ra
  core.mem_w32(kCallee + 8u, 0u);          // delay-slot nop

  nativeOverrideCalls = 0;
  nativeOverrideActiveAddress = 0;
  CHECK(core.nativeDispatcher().install({{image, kCallee}, "native-callee", nativeCallee}));
  core.r[31] = kOuterReturn;

  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(200));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(result.guestPc, kOuterReturn);
  CHECK_EQ(core.pc, kOuterReturn);
  CHECK_EQ(core.r[2], 42u);
  CHECK_EQ(core.r[23], 0u);
  CHECK_EQ(nativeOverrideCalls, 1);
  CHECK_EQ(nativeOverrideActiveAddress, kCallee);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_call_original_runs_guest_body_to_exact_caller_continuation() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  writeReturningCaller(core);
  core.mem_w32(kCallee, 0x2402001fu);      // addiu v0, zero, 31
  core.mem_w32(kCallee + 4u, 0x03e00008u); // jr ra
  core.mem_w32(kCallee + 8u, 0u);          // delay-slot nop

  callOriginalOverrideCalls = 0;
  callOriginalActiveAddress = 0;
  callOriginalActiveAddressAfter = 0;
  callOriginalPcAfter = 0;
  callOriginalPendingWorkAfter = 0;
  callOriginalKey = {image, kCallee};
  callOriginalResult = {};
  CHECK(core.nativeDispatcher().install(
      {callOriginalKey, "native-callee-calling-original", nativeCalleeCallingOriginal}));
  core.r[31] = kOuterReturn;

  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(300));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(result.guestPc, kOuterReturn);
  CHECK_EQ(callOriginalResult.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(callOriginalResult.guestPc, kCaller + 12u);
  CHECK_EQ(callOriginalOverrideCalls, 1);
  CHECK_EQ(callOriginalActiveAddress, kCallee);
  CHECK_EQ(callOriginalActiveAddressAfter, kCallee);
  CHECK_EQ(callOriginalPcAfter, kCallee);
  CHECK_EQ(callOriginalPendingWorkAfter & Core::PW_HOST, Core::PW_HOST);
  CHECK_EQ(core.r[2], 42u);
  CHECK_EQ(core.r[23], 0u);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_nested_native_dispatch_restores_outer_context_and_continuations() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  writeReturningCaller(core);

  outerNativeCalls = 0;
  innerNativeCalls = 0;
  outerActiveAddressBeforeNested = 0;
  outerActiveAddressAfterNested = 0;
  innerActiveAddress = 0;
  outerPcAfterNested = 0;
  nestedNativeResult = {};
  CHECK(core.nativeDispatcher().install({{image, kCallee}, "outer-native", outerNativeCallee}));
  CHECK(core.nativeDispatcher().install({{image, kInnerCallee}, "inner-native", innerNativeCallee}));
  core.r[31] = kOuterReturn;

  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(200));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(result.guestPc, kOuterReturn);
  CHECK_EQ(core.pc, kOuterReturn);
  CHECK_EQ(outerNativeCalls, 1);
  CHECK_EQ(innerNativeCalls, 1);
  CHECK_EQ(outerActiveAddressBeforeNested, kCallee);
  CHECK_EQ(innerActiveAddress, kInnerCallee);
  CHECK_EQ(outerActiveAddressAfterNested, kCallee);
  CHECK_EQ(nestedNativeResult.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(nestedNativeResult.guestPc, kNestedReturn);
  CHECK_EQ(outerPcAfterNested, kCallee);
  CHECK_EQ(core.active_native_address, 0u);
  CHECK_EQ(core.r[2], 42u);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_parked_native_task_keeps_main_and_task_return_boundaries() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  installParkedTaskFixture(core, image, false);
  const auto outer = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(1000));
  const bool mainIntact = outer.returned() && outer.guestPc == kOuterReturn && core.r[23] == 0u &&
                          core.r[18] == kMainRegisterValue && core.r[29] == 0x001fffe0u &&
                          core.active_native_address == 0u && parkedTaskRegisters.r[18] == kTaskRegisterValue;
  std::uint32_t taskNativeAddressAfterResume = 0;
  if (mainIntact) {
    taskNativeAddressAfterResume = resumeParkedTask(core);
  }
  const bool taskIntact = mainIntact && parkedFiber->done() && parkedTaskResult.returned() &&
                          parkedTaskResult.guestPc == kNestedReturn && parkedTaskResumedIntact &&
                          parkedTaskRegisters.r[18] == kTaskRegisterValue && taskNativeAddressAfterResume == 0u &&
                          core.r[18] == kMainRegisterValue;
  parkedFiber->cancel();
  parkedFiber.reset();

  bool cancellationIntact = false;
  if (taskIntact) {
    parkedTaskResult = {};
    parkedTaskResumedIntact = false;
    core.r[31] = kOuterReturn;
    const auto cancelledOuter = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(1000));
    const bool returnedBeforeCancel = cancelledOuter.returned() && cancelledOuter.guestPc == kOuterReturn &&
                                      core.active_native_address == 0u && !parkedFiber->done();
    parkedFiber->cancel(); // longjmp abandons the task's nested BoundarySession on its ending host thread.
    parkedFiber.reset();
    const bool abandonedTaskDidNotResume = !parkedTaskResult.returned() && !parkedTaskResumedIntact;

    core.r[31] = kOuterReturn;
    const auto nextOuter = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(1000));
    const bool returnedAfterCancel = nextOuter.returned() && nextOuter.guestPc == kOuterReturn &&
                                     core.r[18] == kMainRegisterValue && core.active_native_address == 0u;
    parkedFiber->cancel();
    parkedFiber.reset();
    cancellationIntact = returnedBeforeCancel && abandonedTaskDidNotResume && returnedAfterCancel;
  }

  CHECK(mainIntact);
  CHECK(taskIntact);
  CHECK(cancellationIntact);
  CHECK(core.lightrecExecutor().counters().executedBlocks > 0u);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, 0u);
}

static void test_parked_task_fallback_allowance_excludes_main_thread_fallback() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = installTestImage(core);
  installParkedTaskFixture(core, image, true);
  const auto outer = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(1000));
  const bool mainIntact = outer.returned() && outer.guestPc == kOuterReturn &&
                          mainFallbackResult.reason == psx::cpu::ExecutionExitReason::BudgetExhausted &&
                          core.lightrecExecutor().counters().fallback.calls == 1u;
  if (mainIntact) {
    resumeParkedTask(core);
  }
  const bool taskIntact = mainIntact && parkedFiber->done() && parkedTaskResult.returned() &&
                          parkedTaskResult.guestPc == kNestedReturn && parkedTaskResumedIntact &&
                          core.lightrecExecutor().counters().fallback.calls == 2u &&
                          core.lightrecExecutor().counters().fallback.refusedCalls == 0u;
  parkedFiber->cancel();
  parkedFiber.reset();
  exerciseCrossThreadFallback = false;

  CHECK(mainIntact);
  CHECK(taskIntact);
}

static void test_guest_self_modifying_store_invalidates_and_retranslates() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);

  core.mem_w32(kCaller, 0x24020001u);       // addiu v0, zero, 1
  core.mem_w32(kCaller + 4u, 0x03e00008u);  // jr ra
  core.mem_w32(kCaller + 8u, 0u);           // delay-slot nop
  core.mem_w32(kOuterReturn, 0x24177badu);  // must not execute
  core.mem_w32(kWriter, 0x3c080001u);       // lui t0, 1 -- t0 = kCaller
  core.mem_w32(kWriter + 4u, 0x3c092402u);  // lui t1, 0x2402
  core.mem_w32(kWriter + 8u, 0x35290002u);  // ori t1, t1, 2 -- addiu v0, zero, 2
  core.mem_w32(kWriter + 12u, 0xad090000u); // sw t1, 0(t0)
  core.mem_w32(kWriter + 16u, 0x03e00008u); // jr ra
  core.mem_w32(kWriter + 20u, 0u);          // delay-slot nop

  core.r[31] = kOuterReturn;
  const auto first = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(first.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(core.r[2], 1u);
  const auto cacheHitsBeforeReplay = core.lightrecExecutor().counters().cacheHits;

  core.r[2] = 0;
  core.r[31] = kOuterReturn;
  const auto replay = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(replay.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(core.r[2], 1u);
  CHECK(core.lightrecExecutor().counters().cacheHits > cacheHitsBeforeReplay);

  const auto fallbackCallsBeforeWrite = core.lightrecExecutor().counters().fallback.calls;

  core.r[31] = kOuterReturn;
  const auto write = psx::cpu::dispatchGuest(core, kWriter, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(write.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(core.mem_r32(kCaller), 0x24020002u);
  const auto cacheMissesBeforeRetranslate = core.lightrecExecutor().counters().cacheMisses;

  core.r[2] = 0;
  core.r[23] = 0;
  core.r[31] = kOuterReturn;
  const auto second = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
  CHECK_EQ(second.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(core.r[2], 2u);
  CHECK_EQ(core.r[23], 0u);
  CHECK(core.lightrecExecutor().counters().cacheMisses > cacheMissesBeforeRetranslate);
  CHECK_EQ(core.lightrecExecutor().counters().fallback.calls, fallbackCallsBeforeWrite);
}

static void test_pending_host_work_is_serviced_at_a_bounded_execution_exit() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  core.mem_w32(entry, 0x1000ffffu); // beq zero, zero, self
  core.mem_w32(entry + 4u, 0u);     // delay-slot nop
  core.pending_work = Core::PW_HOST;

  const auto result = core.lightrecExecutor().execute(entry, psx::cpu::ExecutionBudget::fromCycles(20));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::HostService);
  CHECK_EQ(core.pending_work & Core::PW_HOST, 0);
  CHECK_EQ(game->timing.guestInstructionTicks, 0u);
}

static void test_deferred_pending_work_does_not_prevent_guest_progress() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x24020007u);      // addiu v0, zero, 7
  core.mem_w32(kCaller + 4u, 0x03e00008u); // jr ra
  core.mem_w32(kCaller + 8u, 0u);          // delay-slot nop
  core.r[31] = kOuterReturn;
  core.pending_work = Core::PW_HOST;
  game->hle.irq_enabled = 0;

  const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(result.guestPc, kOuterReturn);
  CHECK_EQ(core.r[2], 7u);
  CHECK_EQ(core.pending_work & Core::PW_HOST, Core::PW_HOST);
  CHECK(game->timing.guestInstructionTicks > 0u);
}

static void test_invalid_fetch_is_a_typed_hard_fault() {
  Runtime runtime;
  auto game = makeGame(runtime);
  auto &executor = game->core.lightrecExecutor();

  const auto result = executor.execute(0x1a000000u, psx::cpu::ExecutionBudget::fromCycles(20));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(result.guestPc, 0x1a000000u);
  CHECK_EQ(executor.counters().faults, 1u);
  CHECK_EQ(executor.counters().fallback.unsafeInstructionFetch, 1u);
}

static void test_backend_fallback_is_classified_and_counted() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  core.mem_w32(entry, 0x10000001u);      // beq with a branch in its delay slot
  core.mem_w32(entry + 4u, 0x08004003u); // j entry+12 in the delay slot
  core.mem_w32(entry + 8u, 0u);
  core.mem_w32(entry + 12u, 0x1000ffffu); // stable self-loop
  core.mem_w32(entry + 16u, 0u);

  auto &executor = core.lightrecExecutor();
  const auto result = executor.execute(entry, psx::cpu::ExecutionBudget::fromCycles(20));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::BudgetExhausted);
  CHECK(executor.counters().fallback.calls > 0u);
  CHECK(executor.counters().fallback.instructions > 0u);
  CHECK(executor.counters().fallback.unsupportedBlock > 0u);
  CHECK_EQ(executor.counters().fallback.compilationFailed, 0u);
  CHECK_EQ(executor.counters().fallback.unsafeInstructionFetch, 0u);
  CHECK_EQ(executor.counters().fallback.refusedCalls, 0u);
  CHECK(executor.counters().executedBlocks > 0u);

  TelemetryCapture capture;
  executor.reportFallbackTelemetry("positive-test");
  CHECK(telemetryContains(lucent::Level::Warn, "Lightrec fallback telemetry [positive-test]"));
  CHECK(telemetryContains(lucent::Level::Warn, "executor_calls=1"));
  CHECK(telemetryContains(lucent::Level::Warn, "fallback_blocks="));
  CHECK(telemetryContains(lucent::Level::Warn, "fallback_instructions="));
  CHECK(telemetryContains(lucent::Level::Warn, "compilation_failed=0"));
  CHECK(telemetryContains(lucent::Level::Warn, "self_modifying_code=0"));
  CHECK(telemetryContains(lucent::Level::Warn, "unsupported_block="));
  CHECK(telemetryContains(lucent::Level::Warn, "load_delay_hazard=0"));
  CHECK(telemetryContains(lucent::Level::Warn, "unsafe_instruction_fetch=0"));
  CHECK(telemetryContains(lucent::Level::Warn, "refused_fallback_blocks=0"));
  CHECK(telemetryContains(lucent::Level::Warn, "max_fallback_blocks_per_execution=1"));
}

static void test_fallback_threshold_refusal_prevents_interpreter_execution() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  core.mem_w32(entry, 0x10000001u);      // beq with a branch in its delay slot
  core.mem_w32(entry + 4u, 0x08004003u); // j entry+12 in the delay slot
  core.mem_w32(entry + 8u, 0u);
  core.mem_w32(entry + 12u, 0x1000ffffu);
  core.mem_w32(entry + 16u, 0u);

  CHECK(psx::config::set_runtime("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT", "0"));
  TelemetryCapture capture;
  auto &executor = core.lightrecExecutor();
  const auto result = executor.execute(entry, psx::cpu::ExecutionBudget::fromCycles(20));
  CHECK(psx::config::clear_runtime("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT"));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(result.guestPc, entry);
  CHECK_EQ(result.cycles, 0u);
  CHECK(result.detail.find("fallback refused before interpreter execution") != std::string::npos);
  CHECK_EQ(executor.counters().fallback.calls, 0u);
  CHECK_EQ(executor.counters().fallback.instructions, 0u);
  CHECK_EQ(executor.counters().fallback.unsupportedBlock, 0u);
  CHECK_EQ(executor.counters().fallback.refusedCalls, 1u);
  CHECK_EQ(executor.counters().fallback.refusedUnsupportedBlock, 1u);
  CHECK(telemetryContains(lucent::Level::Error, "refused_fallback_blocks=1"));
  CHECK(result.detail.find("admitted_blocks=0, limit=0") != std::string::npos);
}

static void test_invalid_fallback_limit_faults_before_guest_execution() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  core.mem_w32(entry, 0x2402002au); // addiu v0, zero, 42

  CHECK(psx::config::set_runtime("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT", "-1"));
  auto &executor = core.lightrecExecutor();
  const auto result = executor.execute(entry, psx::cpu::ExecutionBudget::fromCycles(20));
  CHECK(psx::config::clear_runtime("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT"));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(result.cycles, 0u);
  CHECK_EQ(core.r[2], 0u);
  CHECK_EQ(executor.counters().fallback.calls, 0u);
  CHECK_EQ(executor.counters().fallback.refusedCalls, 0u);
  CHECK(result.detail.find("expected a non-negative integer") != std::string::npos);
}

int main() {
  RUN(supported_syscall_resumes_function_and_retains_checkpoint_exit);
  RUN(unsupported_syscall_preserves_state_and_refuses_continuation);
  RUN(until_exit_routes_native_entry_syscall_pending_work_and_frame_exit);
  RUN(delay_slot_syscall_is_refused_without_sequential_resume);
  RUN(syscall_continuation_remains_bounded);
  RUN(native_only_self_loop_exhausts_dispatch_budget_without_fabricated_cycles);
  RUN(explicit_function_continuation_is_independent_of_incoming_ra);
  RUN(original_until_exit_suppresses_only_its_entry_and_restores_on_frame_exit);
  RUN(native_store_widths_and_ram_aliases_invalidate_translated_code);
  RUN(nested_original_resumes_native_return_result_not_scoped_caller_pc);
  RUN(backend_reports_verified_host_properties);
  RUN(real_executor_translates_and_runs_guest_instructions);
  RUN(selected_store_observer_bridges_exact_jit_pc_and_rejects_unsupported_target);
  RUN(translated_call_dispatches_image_scoped_native_and_resumes_caller);
  RUN(call_original_runs_guest_body_to_exact_caller_continuation);
  RUN(nested_native_dispatch_restores_outer_context_and_continuations);
  RUN(parked_native_task_keeps_main_and_task_return_boundaries);
  RUN(parked_task_fallback_allowance_excludes_main_thread_fallback);
  RUN(guest_self_modifying_store_invalidates_and_retranslates);
  RUN(pending_host_work_is_serviced_at_a_bounded_execution_exit);
  RUN(deferred_pending_work_does_not_prevent_guest_progress);
  RUN(invalid_fetch_is_a_typed_hard_fault);
  RUN(backend_fallback_is_classified_and_counted);
  RUN(fallback_threshold_refusal_prevents_interpreter_execution);
  RUN(invalid_fallback_limit_faults_before_guest_execution);
  return pt_summary();
}
