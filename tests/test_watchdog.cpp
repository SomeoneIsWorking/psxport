// The frame watchdog must retain its larger boot allowance across bootstrap-image progress until the
// main VRAM presenter COMPLETES, then enforce the shorter steady-state budget. Exercise the shipping API in child
// processes so the deliberate timeout cannot terminate the test runner itself. This is the narrow exception to the
// suite's no-wall-clock rule: the production unit under test is alarm(2), both waits are bounded, and the negative case
// must observe the real signal-handler exit rather than a duplicate timer model.
#include "c_subsys.h"
#include "testutil.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

struct ChildResult {
  int status;
  std::string stderrText;
};

void sleepMilliseconds(long milliseconds) {
  timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000 * 1000};
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
}

std::string readGpuNative() {
  std::string self = __FILE__;
  const size_t slash = self.find_last_of('/');
  const std::string tests = slash == std::string::npos ? "." : self.substr(0, slash);
  FILE *file = fopen((tests + "/../runtime/recomp/gpu_native.cpp").c_str(), "rb");
  if (!file) {
    return {};
  }
  std::string text;
  char buffer[8192];
  size_t count;
  while ((count = fread(buffer, 1, sizeof buffer, file)) != 0) {
    text.append(buffer, count);
  }
  fclose(file);
  return text;
}

enum class ChildAction {
  BootstrapProgressThenComplete,
  CompleteThenStall,
  CompleteProgressThenStall,
};

ChildResult runChild(ChildAction action) {
  int diagnostic[2];
  if (pipe(diagnostic) != 0) {
    return {-1, {}};
  }
  const pid_t child = fork();
  if (child == 0) {
    close(diagnostic[0]);
    dup2(diagnostic[1], STDERR_FILENO);
    close(diagnostic[1]);
    setenv("PSXPORT_WATCHDOG", "1", 1);
    setenv("PSXPORT_WATCHDOG_BOOT", "3", 1);
    watchdog_init();
    if (action == ChildAction::CompleteThenStall) {
      watchdog_main_present_complete();
      pause();
    } else if (action == ChildAction::BootstrapProgressThenComplete) {
      // Longer than the 1s steady budget but shorter than the 3s cold-init grace.
      sleepMilliseconds(1300);
      watchdog_progress();
      sleepMilliseconds(1300);
      watchdog_main_present_complete();
    } else {
      watchdog_main_present_complete();
      sleepMilliseconds(500);
      watchdog_progress();
      pause();
    }
    _exit(0);
  }
  close(diagnostic[1]);
  std::string text;
  char buffer[1024];
  ssize_t count;
  while ((count = read(diagnostic[0], buffer, sizeof buffer)) > 0) {
    text.append(buffer, static_cast<size_t>(count));
  }
  close(diagnostic[0]);
  int status = -1;
  if (child > 0) {
    waitpid(child, &status, 0);
  }
  return {status, text};
}

} // namespace

static void test_bootstrap_progress_retains_grace_until_main_present(void) {
  const ChildResult result = runChild(ChildAction::BootstrapProgressThenComplete);
  CHECK(WIFEXITED(result.status));
  CHECK_EQ(WEXITSTATUS(result.status), 0);
  CHECK(result.stderrText.find("[watchdog] STUCK") == std::string::npos);
}

static void test_completed_present_arms_steady_timeout(void) {
  const ChildResult result = runChild(ChildAction::CompleteThenStall);
  CHECK(WIFEXITED(result.status));
  CHECK_EQ(WEXITSTATUS(result.status), 134);
  CHECK(result.stderrText.find("[watchdog] STUCK") != std::string::npos);
  CHECK(result.stderrText.find("before the main presenter became ready") == std::string::npos);
}

static void test_progress_after_completion_retains_steady_timeout(void) {
  const ChildResult result = runChild(ChildAction::CompleteProgressThenStall);
  CHECK(WIFEXITED(result.status));
  CHECK_EQ(WEXITSTATUS(result.status), 134);
  CHECK(result.stderrText.find("[watchdog] STUCK") != std::string::npos);
  CHECK(result.stderrText.find("before the main presenter became ready") == std::string::npos);
}

static void test_shipping_present_reports_only_after_finalize(void) {
  const std::string source = readGpuNative();
  CHECK(!source.empty());
  const size_t present = source.find("void GpuState::gpu_present_ex(");
  CHECK(present != std::string::npos);
  const size_t finalize = source.find("frame_finalize(core);", present);
  const size_t complete = source.find("watchdog_main_present_complete();", present);
  CHECK(finalize != std::string::npos);
  CHECK(complete != std::string::npos);
  CHECK(complete > finalize);
  CHECK(complete - finalize < 800);
}

int main(void) {
  RUN(bootstrap_progress_retains_grace_until_main_present);
  RUN(completed_present_arms_steady_timeout);
  RUN(progress_after_completion_retains_steady_timeout);
  RUN(shipping_present_reports_only_after_finalize);
  return pt_summary();
}
