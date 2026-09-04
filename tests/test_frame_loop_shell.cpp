// A product frame is advanced only through the framework shell and the title-owned native driver.
// Guest boot code is not an alternative loop owner: a missing driver must refuse before bootInit can
// dispatch a non-returning retail main.
#include "frame_loop_shell.h"
#include "frame_presenter.h"
#include "game.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "testutil.h"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t kVSyncAddress = 0x800859A8u;
constexpr uint32_t kPlatformWindowEnd = 0x80085B20u;

class FenceBackend final : public FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {}
  void captureDiagnostic(uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(uint64_t) override {}
  void beginLedgerFrame() override {}
};

class CountingDriver final : public FrameDriver {
public:
  explicit CountingDriver(int fences) : fences_(fences) {}

  void stepFrame(Core &core, uint32_t frame) override {
    seenCore = &core;
    seenFrame = frame;
    ++calls;
    for (int fence = 0; fence < fences_; ++fence) {
      core.game->presentation.commitUnpresented(backend_);
    }
  }

  Core *seenCore = nullptr;
  uint32_t seenFrame = 0;
  int calls = 0;

private:
  int fences_;
  FenceBackend backend_;
};

class DriverRuntime final : public GameRuntime {
public:
  explicit DriverRuntime(int fences) : fences_(fences) {
    platformPlan_.vsyncAddress = kVSyncAddress;
    platformPlan_.windowLo[0] = kVSyncAddress;
    platformPlan_.windowHi[0] = kPlatformWindowEnd;
  }

  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {
    ++bootCalls;
  }
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  const PlatformHlePlan *platformHlePlan() const override {
    return &platformPlan_;
  }
  std::unique_ptr<FrameDriver> createFrameDriver(Game &) override {
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
  int fences_;
  PlatformHlePlan platformPlan_{};
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
  FrameLoopShell{}.prepareProduct(*game);
}

void step_shell(void *context) {
  auto *game = static_cast<Game *>(context);
  FrameLoopShell{}.step(game->core, 91u);
}

std::string read_source(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

} // namespace

static void test_shell_delegates_exactly_one_native_frame() {
  DriverRuntime runtime(1);
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  CHECK(runtime.createdDriver != nullptr);

  FrameLoopShell shell;
  shell.prepareProduct(*game);
  shell.step(game->core, 73);

  CHECK_EQ(runtime.createdDriver->calls, 1);
  CHECK_EQ(runtime.createdDriver->seenCore, &game->core);
  CHECK_EQ(runtime.createdDriver->seenFrame, 73u);
  CHECK_EQ(runtime.bootCalls, 0);
  CHECK_EQ(game->presentation.fence(), 1u);
}

static void test_missing_driver_refuses_before_boot_can_run() {
  DriverRuntime runtime(-1);
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  expect_abort(require_driver, game.get());
  CHECK_EQ(runtime.bootCalls, 0);
}

static void test_step_before_product_preflight_refuses() {
  DriverRuntime runtime(1);
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  expect_abort(step_shell, game.get());
  CHECK_EQ(runtime.createdDriver->calls, 0);
}

static void test_driver_returning_without_a_fence_refuses() {
  DriverRuntime runtime(0);
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  FrameLoopShell{}.prepareProduct(*game);

  expect_abort(step_shell, game.get());
  CHECK_EQ(game->presentation.fence(), 0u);
}

static void test_driver_committing_twice_refuses() {
  DriverRuntime runtime(2);
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  FrameLoopShell{}.prepareProduct(*game);

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

static void test_native_boot_has_no_title_frame_body_or_fallback() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::string source = read_source(root / "runtime/psx/native_boot.cpp");
  CHECK(!source.empty());

  // These were the title-shaped body and its defining per-frame operations. Native boot may retain
  // host orchestration, but it must delegate through FrameLoopShell instead of growing a fallback.
  CHECK(source.find("pcSched.step(") == std::string::npos);
  CHECK(source.find("hooks->frameUpdate(") == std::string::npos);
  CHECK(source.find("hooks->drawOTag(") == std::string::npos);
  CHECK(source.find("timing.frameTick(") == std::string::npos);
  CHECK(source.find("FrameLoopShell{}.prepareProduct(") != std::string::npos);
  const size_t step = source.find("FrameLoopShell{}.step(");
  const size_t prompt = source.find("repl.consumePromptRequest()");
  CHECK(step != std::string::npos);
  CHECK(prompt != std::string::npos);
  CHECK(prompt > step);
}

int main() {
  RUN(shell_delegates_exactly_one_native_frame);
  RUN(missing_driver_refuses_before_boot_can_run);
  RUN(step_before_product_preflight_refuses);
  RUN(driver_returning_without_a_fence_refuses);
  RUN(driver_committing_twice_refuses);
  RUN(repl_prompt_request_is_one_shot);
  RUN(native_boot_has_no_title_frame_body_or_fallback);
  return pt_summary();
}
