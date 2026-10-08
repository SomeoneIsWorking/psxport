#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "legacy_game_config.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include "scheduler.h"

#include <lucent/log.h>
#include <memory>
#include <string_view>

namespace {

constexpr GameConfig kSyntheticConfig = [] {
  GameConfig config{};
  config.taskTableBase = TASKBASE; // the legacy countdown owner declares this layout
  config.taskSlotStride = TASKSTRIDE;
  config.taskCount = 3u;
  config.curTaskPtr = 0x80021000u;
  return config;
}();

struct CooperativeObservation {
  int cooperativeCalls = 0;
  int cooperativeResumes = 0;
  bool cooperativeContextPreserved = false;
};

class Runtime final : public GameRuntime {
public:
  Runtime() {
    bindLegacyInterface(&kSyntheticConfig, nullptr);
  }
  void *createContext(Core &) override {
    return &nested;
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

  CooperativeObservation nested;
};

void cooperativeNativeOwner(Core *core) {
  auto &observed = *static_cast<CooperativeObservation *>(core->gameCtx);
  ++observed.cooperativeCalls;
  const std::uint32_t stack = core->r[29];
  const std::uint32_t activeAddress = core->active_native_address;
  core->r[16] = 0xa0b0c0d0u;
  const std::uint32_t base = core->cfg->taskTableBase + core->cfg->taskSlotStride;
  core->mem_w16(base, 1u);
  core->mem_w16(base + 2u, 1u);
  scheduler_yield(core);
  ++observed.cooperativeResumes;
  observed.cooperativeContextPreserved =
      core->r[16] == 0xa0b0c0d0u && core->r[29] == stack && core->active_native_address == activeAddress;
}

class Checks {
public:
  void require(bool condition, const char *name) {
    ++checked_;
    if (!condition) {
      ++failed_;
      lucent::error("guest-task-lifecycle-test", "failed: {}", name);
    }
  }

  int finish() const {
    lucent::info("guest-task-lifecycle-test", "checked={} failed={}", checked_, failed_);
    return failed_ == 0 ? 0 : 1;
  }

private:
  int checked_ = 0;
  int failed_ = 0;
};

void checkCooperativeNativeYield(Game &game, Runtime &runtime, Checks &checks) {
  Core &core = game.core;
  constexpr std::uint32_t entry = 0x80013000u;
  constexpr std::uint32_t callback = entry + 64u;
  constexpr int slot = 1;
  const std::uint32_t base = core.cfg->taskTableBase + slot * core.cfg->taskSlotStride;
  const std::uint32_t instructions[] = {
      0x27bdfff0u,
      0xafbf000cu,
      0x0c000000u | ((callback >> 2u) & 0x03ffffffu),
      0u,
      0x8fbf000cu,
      0u,
      0x27bd0010u,
      0x03e00008u,
      0u,
  };
  std::uint32_t address = entry;
  for (const std::uint32_t instruction : instructions) {
    core.mem_w32(address, instruction);
    address += 4u;
  }
  const auto image = core.imageCatalog().activate("cooperative-native-task", {0x13000u, 0x13080u}, 3u);
  checks.require(core.nativeDispatcher().install({{image, callback}, "cooperative-native", cooperativeNativeOwner}),
                 "cooperative native callback installs through shipping dispatch");
  core.mem_w16(base, 2u);
  core.mem_w32(base + 8u, 0x801ff000u);
  core.mem_w32(base + 12u, entry);
  const R3000 caller = core;
  guest_run_coro_fiber_stanza(&core, slot, base, 2u, false, caller);
  checks.require(core.mem_r16(base) == 1u && runtime.nested.cooperativeCalls == 1 &&
                     runtime.nested.cooperativeResumes == 0,
                 "cooperative yield suspends the existing nested native call");
  game.pcSched.tickSleepCountdown();
  core.r[16] = 0u; // unrelated host work cannot replace the saved task's registers
  guest_run_coro_fiber_stanza(&core, slot, base, core.mem_r16(base), false, caller);
  checks.require(runtime.nested.cooperativeResumes == 1 && runtime.nested.cooperativeContextPreserved,
                 "cooperative resume preserves the suspended native stack and context");
  checks.require(core.mem_r16(base) == 0u && game.pcSched.coro[slot] == nullptr && !core.executionControl().pending(),
                 "cooperative task completes and releases its coroutine without a pending exit");
}

void checkGuestTaskBudgetLifetime(Game &game, Checks &checks) {
  Core &core = game.core;
  constexpr std::uint32_t entry = 0x80011000u;
  constexpr std::uint32_t completedCount = 0x80012000u;
  constexpr std::uint32_t iterations = 1'000'000u;
  constexpr std::uint32_t taskStack = 0x801ff000u;
  constexpr int slot = 1;
  const std::uint32_t base = core.cfg->taskTableBase + slot * core.cfg->taskSlotStride;
  // A finite synthetic workload that cannot finish within one shipping turn.
  const std::uint32_t instructions[] = {
      0x27bdfff0u,                                       // addiu sp, sp, -16
      0xafbf000cu,                                       // sw ra, 12(sp)
      0x0c000000u | ((entry + 48u) >> 2u & 0x03ffffffu), // jal finite worker
      0u,
      0x8fbf000cu, // lw ra, 12(sp)
      0u,          // load delay
      0x27bd0010u, // addiu sp, sp, 16
      0x3c098001u, // lui t1, 0x8001
      0xad222000u, // sw v0, 0x2000(t1)
      0x03e00008u, // jr ra: original task return sentinel
      0u,
      0u,
      0x3c080000u | (iterations >> 16),     // lui t0, upper iteration count
      0x35080000u | (iterations & 0xffffu), // ori t0, t0, lower iteration count
      0x24020000u,                          // addiu v0, zero, 0
      0x2508ffffu,                          // addiu t0, t0, -1
      0x1500fffeu,                          // bne t0, zero, decrement
      0x24420001u,                          // delay slot: addiu v0, v0, 1
      0x03e00008u,                          // jr ra: resume the task's outer frame
      0u,
  };
  std::uint32_t address = entry;
  for (const std::uint32_t instruction : instructions) {
    core.mem_w32(address, instruction);
    address += 4u;
  }
  core.imageCatalog().activate("finite-budget-task", {entry & 0x1fffffffu, address & 0x1fffffffu}, 2u);
  core.mem_w16(base, 2u);
  core.mem_w32(base + 8u, taskStack);
  core.mem_w32(base + 12u, entry);
  const R3000 caller = core;
  guest_run_coro_fiber_stanza(&core, slot, base, 2u, false, caller);
  const bool parked = core.mem_r16(base) == 2u && game.pcSched.coro[slot] != nullptr;
  checks.require(parked, "an exhausted guest task remains runnable with its live coroutine");
  checks.require(!core.executionControl().pending(), "task budget exhaustion does not escape into the frame caller");
  checks.require(core.mem_r32(completedCount) == 0u, "first turn stopped before the finite guest workload completed");

  if (!parked) {
    return;
  }

  std::uint32_t previousCount = 0;
  unsigned turns = 1;
  while (core.mem_r16(base) != 0u && turns < 32u) {
    const R3000 &saved = game.pcSched.task_ctx[slot];
    checks.require(saved.r[29] == taskStack - 16u && saved.r[31] == entry + 16u && saved.pc >= entry &&
                       saved.pc < address,
                   "budget suspension retains the nested task stack, live return address and exact guest PC");
    checks.require(saved.r[2] > previousCount, "successive budgets advance instead of restarting the task");
    previousCount = saved.r[2];
    guest_run_coro_fiber_stanza(&core, slot, base, core.mem_r16(base), false, caller);
    ++turns;
  }
  checks.require(turns > 2u && core.mem_r16(base) == 0u && game.pcSched.coro[slot] == nullptr,
                 "finite workload returns and releases its coroutine after multiple budgets");
  checks.require(core.mem_r32(completedCount) == iterations && core.r[29] == taskStack,
                 "all iterations execute exactly once across scheduler resumptions");
  checks.require(!core.executionControl().pending(), "completed task leaves no unrelated pending execution exit");
  checks.require(core.lightrecExecutor().counters().executedBlocks > 0 &&
                     core.lightrecExecutor().counters().fallback.calls == 0,
                 "multi-budget task executes through the JIT with zero fallback");

  core.mem_w32(completedCount, 0u);
  core.mem_w16(base, 2u);
  guest_run_coro_fiber_stanza(&core, slot, base, 2u, false, caller);
  checks.require(core.mem_r16(base) == 2u && game.pcSched.coro[slot] != nullptr,
                 "second workload reaches a live budget suspension before cancellation");
  constexpr std::uint32_t replacement = 0x80014000u;
  core.mem_w32(replacement, 0x2402002au);      // addiu v0, zero, 42
  core.mem_w32(replacement + 4u, 0x03e00008u); // jr ra
  core.mem_w32(replacement + 8u, 0u);
  core.imageCatalog().activate("replacement-task", {0x14000u, 0x1400cu}, 4u);
  core.mem_w32(base + 12u, replacement);
  core.mem_w16(base, 3u); // authored scheduler restart cancels the previous suspended coroutine
  guest_run_coro_fiber_stanza(&core, slot, base, 3u, false, caller);
  checks.require(core.r[2] == 42u && core.mem_r16(base) == 0u && game.pcSched.coro[slot] == nullptr,
                 "restart cancels the budget suspension and executes the replacement task");
  checks.require(core.mem_r32(completedCount) == 0u && !core.executionControl().pending() &&
                     core.active_native_address == 0u,
                 "budget cancellation cannot complete the canceled work or leave an executor/native guard active");
}

void checkGuestAuthoredTaskState(Game &game, Checks &checks) {
  Core &core = game.core;
  constexpr std::uint32_t entry = 0x80015000u;
  constexpr std::uint32_t completed = 0x80016000u;
  constexpr int slot = 1;
  const std::uint32_t base = core.cfg->taskTableBase + slot * core.cfg->taskSlotStride;
  // Set the authored state before a finite loop crosses the budget boundary.
  // a0 selects cancellation, sleep or restart; no native yield has executed yet.
  const std::uint32_t instructions[] = {
      0x3c090000u | (base >> 16u),
      0x35290000u | (base & 0xffffu),
      0xa5240000u, // sh a0, 0(t1)
      0x3c08000fu, // lui t0, 15
      0x35084240u, // ori t0, t0, 16960: one million iterations
      0x2508ffffu, // addiu t0, t0, -1
      0x1500fffeu, // bne t0, zero, decrement
      0u,
      0x3c098001u, // lui t1, 0x8001
      0xad246000u, // sw a0, 0x6000(t1): reached only after completion
      0x03e00008u,
      0u,
  };
  std::uint32_t address = entry;
  for (const std::uint32_t instruction : instructions) {
    core.mem_w32(address, instruction);
    address += 4u;
  }
  core.imageCatalog().activate("authored-state-task", {0x15000u, address & 0x1fffffffu}, 5u);
  for (const std::uint32_t authoredState : {0u, 1u, 3u}) {
    core.mem_w32(completed, 0xffffffffu);
    core.mem_w16(base, 2u);
    core.mem_w16(base + 2u, 2u);
    core.mem_w32(base + 8u, 0x801ff000u);
    core.mem_w32(base + 12u, entry);
    R3000 caller = core;
    caller.r[4] = authoredState;
    guest_run_coro_fiber_stanza(&core, slot, base, 2u, false, caller);
    checks.require(core.mem_r16(base) == authoredState,
                   "budget boundary preserves the state written by the running guest");
    checks.require(core.mem_r32(completed) == 0xffffffffu && !core.executionControl().pending(),
                   "authored transition reaches the budget boundary before task completion without leaking its exit");
    if (authoredState == 0u) {
      checks.require(game.pcSched.coro[slot] == nullptr && game.pcSched.task_started[slot] == 0,
                     "guest-written cancellation releases the task at the first budget boundary");
      const auto calls = core.lightrecExecutor().counters().calls;
      guest_run_coro_fiber_stanza(&core, slot, base, core.mem_r16(base), false, caller);
      checks.require(core.lightrecExecutor().counters().calls == calls && core.mem_r32(completed) == 0xffffffffu,
                     "canceled guest task cannot execute another JIT turn");
    } else if (authoredState == 1u) {
      const auto calls = core.lightrecExecutor().counters().calls;
      game.pcSched.tickSleepCountdown();
      guest_run_coro_fiber_stanza(&core, slot, base, core.mem_r16(base), false, caller);
      checks.require(core.mem_r16(base) == 1u && core.lightrecExecutor().counters().calls == calls,
                     "authored sleep remains suspended until the countdown owner rearms it");
      game.pcSched.tickSleepCountdown();
      checks.require(core.mem_r16(base) == 2u, "existing countdown owner rearms the sleeping task");
      for (unsigned turn = 0; turn < 32u && core.mem_r16(base) == 2u; ++turn) {
        guest_run_coro_fiber_stanza(&core, slot, base, 2u, false, caller);
      }
      checks.require(core.mem_r16(base) == 0u && core.mem_r32(completed) == 1u && game.pcSched.coro[slot] == nullptr,
                     "guest-authored sleep resumes the retained task through its real return");
    } else {
      core.mem_w32(base + 12u, 0x80014000u); // replacement installed by the preceding lifetime test
      guest_run_coro_fiber_stanza(&core, slot, base, core.mem_r16(base), false, caller);
      checks.require(core.r[2] == 42u && core.mem_r16(base) == 0u && game.pcSched.coro[slot] == nullptr &&
                         core.mem_r32(completed) == 0xffffffffu,
                     "guest-authored restart reaches the existing replacement owner without completing old work");
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  Runtime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  if (argc == 2 && std::string_view(argv[1]) == "--terminal-fault") {
    constexpr std::uint32_t unmappedEntry = 0x80018000u;
    constexpr int slot = 1;
    const std::uint32_t base = core.cfg->taskTableBase + slot * core.cfg->taskSlotStride;
    core.mem_w16(base, 2u);
    core.mem_w32(base + 8u, 0x801ff000u);
    core.mem_w32(base + 12u, unmappedEntry);
    const R3000 caller = core;
    guest_run_coro_fiber_stanza(&core, slot, base, 2u, false, caller);
    lucent::error("guest-task-lifecycle-test", "scheduler returned after a terminal task fault");
    return 0; // The subprocess regression requires failure at the scheduler boundary.
  }
  Checks checks;
  checkCooperativeNativeYield(*game, runtime, checks);
  checkGuestTaskBudgetLifetime(*game, checks);
  checkGuestAuthoredTaskState(*game, checks);
  return checks.finish();
}
