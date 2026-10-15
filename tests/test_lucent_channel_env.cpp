// PSXPORT_DEBUG and PSXPORT_LOG_FILE take effect through lucent alone, with no psxport setup run.
//
// THREE CASES, because they fail for different reasons and none implies the others:
//   1. first-in-process — the first logging call of any kind reads the variable.
//   2. before main()    — the same, from a static initialiser in a freshly exec'd child, where no
//                         line of this program's own setup has run at all. That is the case a
//                         "just call an init function early" design cannot satisfy, and it is why
//                         the name is a build-time setting in lucent rather than a setter.
//   3. PSXPORT_LOG_FILE — lucent's log file redirect.
//
// WHAT A NEGATIVE PRINTS: both cases log on TWO channels of which PSXPORT_DEBUG names ONE, and both
// assert the captured line count and the exact text, dumping whatever was captured. "0 lines"
// (variable ignored) and "2 lines" (everything switched on) are each distinguishable from a pass.
#include "testutil.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <string>
#include <vector>

namespace {

bool is_timestamped_line(std::string_view line, std::string_view payload) {
  // Lucent's sink and file contracts include its UTC timestamp. Check its fixed shape while
  // keeping the wall-clock portion dynamic, then require the exact channel/message payload.
  constexpr std::size_t kTimestampLength = 24;                // YYYY-MM-DDTHH:MM:SS.mmmZ
  constexpr std::size_t kPrefixLength = kTimestampLength + 3; // '[' + timestamp + "] "
  if (line.size() < kPrefixLength || line[0] != '[' || line[kTimestampLength + 1] != ']' ||
      line[kTimestampLength + 2] != ' ') {
    return false;
  }
  for (const std::size_t index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u, 20u, 21u, 22u}) {
    if (!std::isdigit(static_cast<unsigned char>(line[index + 1]))) {
      return false;
    }
  }
  return line[5] == '-' && line[8] == '-' && line[11] == 'T' && line[14] == ':' && line[17] == ':' && line[20] == '.' &&
         line[24] == 'Z' && line.substr(kPrefixLength) == payload;
}

// Set only in the child process spawned by case 2. Checked with getenv — NOT with lucent::config —
// so that in the parent this whole block touches nothing in lucent and the parent's first lucent
// call really is the one in case 1.
bool is_pre_main_child(void) {
  return getenv("PSXPORT_TEST_PREMAIN_CHILD") != nullptr;
}

// ── the pre-main half, active only in that child ────────────────────────────────────────────────
// Dynamic initialisation inside ONE translation unit runs in declaration order, so: the vector, then
// the sink, then the debug calls — with nothing else in the process having executed.
std::vector<std::string> g_early_lines;

struct InstallEarlySink {
  InstallEarlySink() noexcept {
    if (!is_pre_main_child()) {
      return;
    }
    lucent::set_sink([](lucent::Level, std::string_view line) {
      g_early_lines.emplace_back(line);
    });
  }
};
const InstallEarlySink g_early_sink;

struct EmitEarlyLines {
  EmitEarlyLines() noexcept {
    if (!is_pre_main_child()) {
      return;
    }
    lucent::debug("tbomb", "pre-main line on an enabled channel");
    lucent::debug("tbomb-off", "pre-main line on a channel that is NOT enabled");
  }
};
const EmitEarlyLines g_early_lines_emitted;

// Exit 0 only if exactly the one enabled line was captured before main.
int run_as_child(void) {
  fprintf(stderr,
          "  child: PSXPORT_DEBUG=%s captured %zu pre-main line(s)\n",
          getenv("PSXPORT_DEBUG") ? getenv("PSXPORT_DEBUG") : "<unset>",
          g_early_lines.size());
  for (const std::string &l : g_early_lines) {
    fprintf(stderr, "  child: | %s\n", l.c_str());
  }
  lucent::set_sink(nullptr);
  if (g_early_lines.size() != 1) {
    return 1;
  }
  return is_timestamped_line(g_early_lines[0], "[tbomb] pre-main line on an enabled channel") ? 0 : 1;
}

// ── case 1: the first logging call in this process ──────────────────────────────────────────────
void test_psxport_debug_is_honoured_by_the_first_log_call(void) {
  // Set BEFORE anything in this process has read config or logged. From here on the only question is
  // whether lucent ever looks at this name.
  CHECK_EQ(setenv("PSXPORT_DEBUG", "tbomb", 1), 0);

  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });

  lucent::debug("tbomb", "sector {} -> {:08X}", 17, 0x80010000u);
  lucent::debug("tbomb-off", "must not appear");

  fprintf(stderr,
          "  captured %zu line(s) from 2 debug() calls on 2 channels, 1 of them named by "
          "PSXPORT_DEBUG=%s\n",
          lines.size(),
          getenv("PSXPORT_DEBUG"));
  for (const std::string &l : lines) {
    fprintf(stderr, "  | %s\n", l.c_str());
  }

  lucent::set_sink(nullptr);
  CHECK_EQ((int)lines.size(), 1);
  CHECK(lines.size() == 1 && is_timestamped_line(lines[0], "[tbomb] sector 17 -> 80010000"));
}

// ── case 2: before main(), in a child process with no setup at all ──────────────────────────────
void test_psxport_debug_is_honoured_before_main(void) {
  char self[4096];
  const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  CHECK(n > 0);
  if (n <= 0) {
    return;
  }
  self[n] = 0;

  const pid_t child = fork();
  if (child == 0) {
    setenv("PSXPORT_DEBUG", "tbomb", 1);
    setenv("PSXPORT_TEST_PREMAIN_CHILD", "1", 1);
    execl(self, self, static_cast<char *>(nullptr));
    _exit(127);
  }
  int rc = -1;
  if (child > 0) {
    waitpid(child, &rc, 0);
  }
  fprintf(stderr, "  child exited with wait status %d (0 = the pre-main line was emitted)\n", rc);
  CHECK_EQ(rc, 0);
}

// ── PSXPORT_LOG_FILE ─────────────────────────────────────────────────────────────────────────────
// lucent's LUCENT_LOG_FILE_ENV. This must run LAST: lucent resolves its output stream on the first line that is not
// going to a sink, and does not re-open it afterwards.
void test_psxport_log_file_still_redirects_output(void) {
  char path[512];
  snprintf(path, sizeof path, "psxport_log_file_test_%d.log", (int)getpid());
  remove(path);
  CHECK_EQ(setenv("PSXPORT_LOG_FILE", path, 1), 0);
  lucent::set_sink(nullptr); // no sink: output must go to the file, not stderr

  lucent::info("tbomb", "redirected line {}", 42);

  const std::string text = psx::test::readFile(path);
  remove(path);
  fprintf(stderr, "  %s holds %zu byte(s): %s", path, text.size(), text.empty() ? "<empty>\n" : text.c_str());
  CHECK(is_timestamped_line(text, "[tbomb] redirected line 42\n"));
}

} // namespace

int main(void) {
  if (is_pre_main_child()) {
    return run_as_child();
  }
  (void)g_early_lines_emitted;
  RUN(psxport_debug_is_honoured_by_the_first_log_call);
  RUN(psxport_debug_is_honoured_before_main);
  RUN(psxport_log_file_still_redirects_output);
  return psx::test::summary();
}
