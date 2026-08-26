// A transition-to-black present is visible progress, but it is not proof that the main gameplay
// presenter is ready. Exercise the shipping GpuState::gpu_clear_display path in a child process so
// the deliberate watchdog timeout cannot terminate the test runner. The no-blit form preserves all
// clear/present/finalize/watchdog bookkeeping while keeping this test hermetic: no SDL device,
// window, disc, or game assets are initialized.
#include "c_subsys.h"
#include "game.h"
#include "testutil.h"

#include <errno.h>
#include <signal.h>
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

ChildResult runClearDisplayChild() {
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

    static Game game;
    game.core.rsub.mode.setPath(RenderPath::Psx);
    watchdog_init();
    game.gpu.gpu_clear_display(&game.core, /*do_blit=*/0);

    // Longer than the steady budget but shorter than the cold-init grace. If the black transition
    // falsely reports main-present completion, the child exits 134 before reaching _exit(0).
    sleepMilliseconds(1300);
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

static void test_clear_display_retains_boot_grace(void) {
  const ChildResult result = runClearDisplayChild();
  CHECK(WIFEXITED(result.status));
  CHECK_EQ(WEXITSTATUS(result.status), 0);
  CHECK(result.stderrText.find("[watchdog] STUCK") == std::string::npos);
}

int main() {
  RUN(clear_display_retains_boot_grace);
  return pt_summary();
}
