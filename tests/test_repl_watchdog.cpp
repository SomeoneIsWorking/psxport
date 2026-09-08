// Inspect the real watchdog timer inside the shipping REPL input boundary, without a timed wait.
#include "c_subsys.h"
#include "config_vars.h"
#include "repl.h"
#include "testutil.h"

#include <algorithm>
#include <cstring>
#include <sys/time.h>

namespace {

const char *nextCommand;
int inputCalls;
bool inputWasSuspended;

bool readCommand(std::span<char> line) {
  ++inputCalls;
  itimerval timer{};
  const bool queried = getitimer(ITIMER_REAL, &timer) == 0;
  inputWasSuspended = queried && timer.it_value.tv_sec == 0 && timer.it_value.tv_usec == 0;
  if (!nextCommand) {
    return false;
  }
  const size_t count = std::min(std::strlen(nextCommand), line.size() - 1);
  std::copy_n(nextCommand, count, line.data());
  line[count] = '\0';
  return true;
}

class WatchdogFixture {
public:
  explicit WatchdogFixture(bool steady) {
    psx::config::cv_watchdog.set(psx::config::Layer::Runtime, 3);
    psx::config::cv_watchdog_boot.set(psx::config::Layer::Runtime, 9);
    watchdog_init();
    if (steady) {
      watchdog_main_present_complete();
    }
    inputCalls = 0;
    inputWasSuspended = false;
  }
  ~WatchdogFixture() {
    watchdog_disable();
  }
};

void checkInputLifecycle(const char *command, long expectedResult, bool steady) {
  WatchdogFixture fixture(steady);
  nextCommand = command;
  Repl repl;
  const long result = repl.read(nullptr, 913u, readCommand);
  CHECK_EQ(result, expectedResult);
  CHECK_EQ(inputCalls, 1);
  CHECK(inputWasSuspended);

  itimerval timer{};
  CHECK_EQ(getitimer(ITIMER_REAL, &timer), 0);
  CHECK(timer.it_value.tv_sec > 0 || timer.it_value.tv_usec > 0);
  CHECK(timer.it_value.tv_sec <= (steady ? 3 : 9));
  CHECK(timer.it_value.tv_sec >= (steady ? 2 : 8));
}

void test_run_resumes_boot_watchdog_without_claiming_present() {
  checkInputLifecycle("run 4\n", 4, false);
}

void test_step_resumes_steady_watchdog() {
  checkInputLifecycle("step\n", 1, true);
}

void test_quit_and_end_restore_watchdog() {
  checkInputLifecycle("quit\n", -1, true);
  checkInputLifecycle("end\n", -2, true);
}

void test_eof_restores_watchdog() {
  checkInputLifecycle(nullptr, -1, true);
}

} // namespace

int main() {
  RUN(run_resumes_boot_watchdog_without_claiming_present);
  RUN(step_resumes_steady_watchdog);
  RUN(quit_and_end_restore_watchdog);
  RUN(eof_restores_watchdog);
  return pt_summary();
}
