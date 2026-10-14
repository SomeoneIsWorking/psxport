// A product frame is advanced only through the framework shell and the title-owned native driver.
// Guest boot code is not an alternative loop owner: a missing driver must refuse before startProgram can
// dispatch a non-returning retail main.
#include "frame_loop_shell.h"
#include "frame_presenter.h"
#include "game.h"
#include "game_runtime.h"
#include "gte_state.h"
#include "platform_hle.h"
#include "proj_params.h"
#include "proj_prim.h"
#include "stock_vsync.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <csignal>
#include <cstdint>
#include <memory>
#include <span>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t kVSyncAddress = 0x800859A8u;

class FenceBackend final : public psx::frame::FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {}
  void captureDiagnostic(uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(uint64_t) override {}
  void beginLedgerFrame() override {}
  bool interpolatesRecords() const override {
    return false;
  }
  bool sealedFrameIsCut() override {
    return false;
  }
  void presentInBetween() override {}
  psx::gpu::RecordRect displayedBuffer() override {
    return {};
  }
};

class CountingDriver final : public psx::game::FrameDriver {
public:
  explicit CountingDriver(int fences) : fences_(fences) {}

  void stepFrame(Core &core, uint32_t frame) override {
    seenCore = &core;
    seenFrame = frame;
    stepsOnBoundCore += ProjPrim::current() == &core.rsub.projprim ? 1 : 0;
    ++calls;
    for (int fence = 0; fence < fences_; ++fence) {
      core.game->presentation.commitUnpresented(backend_);
    }
  }

  Core *seenCore = nullptr;
  uint32_t seenFrame = 0;
  int calls = 0;
  int stepsOnBoundCore = 0;

private:
  int fences_;
  FenceBackend backend_;
};

class DriverRuntime final : public psx::test::StubGameRuntime {
public:
  explicit DriverRuntime(int fences) : fences_(fences) {}

  void registerOverrides(Game &game) override {
    vsync_.bind(game);
  }
  void startProgram(Core &) override {
    ++bootCalls;
  }
  std::unique_ptr<psx::game::FrameDriver> createFrameDriver(Game &) override {
    if (fences_ < 0) {
      return nullptr;
    }
    auto driver = std::make_unique<CountingDriver>(fences_);
    createdDriver = driver.get();
    return driver;
  }

  CountingDriver *createdDriver = nullptr;
  int bootCalls = 0;

private:
  const psx::hle::StockVSync vsync_{kVSyncAddress, 0u};
  int fences_;
};

void expect_abort(void (*operation)(void *), void *context) {
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    operation(context);
    _exit(0);
  }
  int status = 0;
  CHECK_EQ(waitpid(child, &status, 0), child);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGABRT);
}

void require_driver(void *context) {
  auto *game = static_cast<Game *>(context);
  psx::frame::FrameLoopShell{}.prepareProduct(*game);
}

void step_shell(void *context) {
  auto *game = static_cast<Game *>(context);
  psx::frame::FrameLoopShell{}.step(game->core, 91u);
}

} // namespace

static void test_shell_delegates_exactly_one_native_frame() {
  DriverRuntime runtime(1);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);
  CHECK(runtime.createdDriver != nullptr);

  psx::frame::FrameLoopShell shell;
  shell.prepareProduct(*game);
  shell.step(game->core, 73);

  CHECK_EQ(runtime.createdDriver->calls, 1);
  CHECK_EQ(runtime.createdDriver->seenCore, &game->core);
  CHECK_EQ(runtime.createdDriver->seenFrame, 73u);
  CHECK_EQ(runtime.bootCalls, 0);
  CHECK_EQ(game->presentation.fence(), 1u);
}

namespace {

// Another session's boot binds the devices to its own Core; the next step of this one rebinds them.
void test_a_step_rebinds_after_another_core_was_bound() {
  DriverRuntime runtime(1);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);
  CountingDriver *driver = runtime.createdDriver;
  auto other = std::make_unique<Game>(runtime);
  psx::frame::FrameLoopShell shell;
  shell.prepareProduct(*game);
  shell.step(game->core, 1);
  shell.bindRunningCore(other->core);
  shell.step(game->core, 2);
  CHECK_EQ(driver->calls, 2);
  CHECK_EQ(driver->stepsOnBoundCore, 2);
}

// Steps of the Core the shell bound last leave the bindings alone.
void test_a_step_of_the_bound_core_does_not_rebind() {
  DriverRuntime runtime(1);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);
  CountingDriver *driver = runtime.createdDriver;
  auto other = std::make_unique<Game>(runtime);
  psx::frame::FrameLoopShell shell;
  shell.prepareProduct(*game);
  shell.step(game->core, 1);
  other->core.rsub.projprim.bind(&other->core);
  shell.step(game->core, 2);
  CHECK_EQ(driver->calls, 2);
  CHECK_EQ(driver->stepsOnBoundCore, 1);
}

// A forgotten Core is rebound on its next step, even when it is the one bound last.
void test_a_forgotten_core_is_rebound() {
  DriverRuntime runtime(1);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);
  CountingDriver *driver = runtime.createdDriver;
  auto other = std::make_unique<Game>(runtime);
  psx::frame::FrameLoopShell shell;
  shell.prepareProduct(*game);
  shell.step(game->core, 1);
  other->core.rsub.projprim.bind(&other->core);
  shell.forgetCore(game->core);
  shell.step(game->core, 2);
  CHECK_EQ(driver->stepsOnBoundCore, 2);
}

// Destroying a Game that is not bound leaves the bound Game's peripherals in place.
void test_destroying_an_unbound_game_keeps_the_bound_one() {
  DriverRuntime runtime(1);
  auto game = std::make_unique<Game>(runtime);
  auto other = std::make_unique<Game>(runtime);
  psx::frame::FrameLoopShell shell;
  shell.bindRunningCore(game->core);
  other.reset();
  CHECK(GTE_CurState() == &game->core.gte().registers());
  CHECK(ProjParams::current() == &game->core.rsub.projParams);
  CHECK(ProjPrim::current() == &game->core.rsub.projprim);
  CHECK(game->spu.isBound());
  CHECK(game->mdec.isBound());
  const GteRegs *registers = &game->core.gte().registers();
  game.reset();
  CHECK(GTE_CurState() != registers);
  CHECK(ProjParams::current() == nullptr);
  CHECK(ProjPrim::current() == nullptr);
}

} // namespace

static void test_missing_driver_refuses_before_boot_can_run() {
  DriverRuntime runtime(-1);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);

  expect_abort(require_driver, game.get());
  CHECK_EQ(runtime.bootCalls, 0);
}

static void test_step_before_product_preflight_refuses() {
  DriverRuntime runtime(1);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);

  expect_abort(step_shell, game.get());
  CHECK_EQ(runtime.createdDriver->calls, 0);
}

static void test_driver_returning_without_a_fence_refuses() {
  DriverRuntime runtime(0);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);
  psx::frame::FrameLoopShell{}.prepareProduct(*game);

  expect_abort(step_shell, game.get());
  CHECK_EQ(game->presentation.fence(), 0u);
}

static void test_driver_committing_twice_refuses() {
  DriverRuntime runtime(2);
  auto game = std::make_unique<Game>(runtime);
  runtime.registerOverrides(*game);
  psx::frame::FrameLoopShell{}.prepareProduct(*game);

  expect_abort(step_shell, game.get());
  CHECK_EQ(game->presentation.fence(), 0u);
}

static void test_repl_prompt_request_is_one_shot() {
  Repl repl;
  CHECK(!repl.consumePromptRequest());

  repl.requestPrompt();
  CHECK(repl.consumePromptRequest());
  CHECK(!repl.consumePromptRequest());
}

int main() {
  RUN(shell_delegates_exactly_one_native_frame);
  RUN(a_step_rebinds_after_another_core_was_bound);
  RUN(a_step_of_the_bound_core_does_not_rebind);
  RUN(a_forgotten_core_is_rebound);
  RUN(destroying_an_unbound_game_keeps_the_bound_one);
  RUN(missing_driver_refuses_before_boot_can_run);
  RUN(step_before_product_preflight_refuses);
  RUN(driver_returning_without_a_fence_refuses);
  RUN(driver_committing_twice_refuses);
  RUN(repl_prompt_request_is_one_shot);
  return pt_summary();
}
