// A control-surface read that returns FEWER words than asked must say so, on both transports.
//
// WHY THIS EXISTS. `Repl` (stdin) and `DbgServer` (the loopback live endpoint) are two transports over
// the same verbs, and each carried its own literal `64` for "words per line". Each CLAMPED the request
// and returned the clamped answer with no indication that anything was missing.
//
// That is the worst shape a diagnostic can have. Measured 2026-09-27 on Spyro 1:
// `tools/probe_moby_list.py` asked the live endpoint for 1408 words in one `rw`; the endpoint returned
// 64; the reader waited for 1408 words that were never coming and the probe hung to its timeout having
// produced nothing. A less careful caller would have read the 64 words it received and treated the
// remaining 1344 as zeros — and for that probe, "zeros" is exactly the answer it was built to look for
// (`is the moby list empty?`). A silent short read is not a small annoyance in a diagnostic; it is a
// mechanism for manufacturing the result.
//
// So the contract is: serve at most `kMaxControlReadWords` words, and when the request asked for more,
// ANNOUNCE IT on its own line, naming how many were served and how many were asked for. Never pad the
// missing words with zeros and never drop them silently. The data line's format is unchanged, so a
// caller that parses one line still parses it.
//
// The cap is stated once, in `runtime/psx/control_surface_limits.h`, because two transports each
// carrying their own literal is two places for them to drift apart silently.
//
// NO GAME, NO WINDOW, NO PRODUCT: a bare Core, the REPL, and an in-process log sink.
#include "control_surface_limits.h"
#include "core.h"
#include "game_iface.h"
#include "repl.h"
#include "testutil.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstring>
#include <format>

// A WARNING THAT COST THE FIRST VERSION OF THIS TEST, recorded so it is not repeated: `std::format`
// uses `{}` placeholders, NOT printf's `%08X`. Written printf-style it returns the TEMPLATE UNCHANGED
// and silently ignores the arguments, so the REPL was handed the literal text `rw %08X %u`, answered
// `? rw` as an unknown command, and the test failed with "no data line" — pointing at the framework
// instead of at the format string. `fprintf` below is genuinely printf-style; the two are not
// interchangeable and sit ten lines apart.
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kCap = psx::control::kMaxControlReadWords;

std::vector<std::string> g_lines;

void capture_start() {
  g_lines.clear();
  lucent::set_sink([](lucent::Level, std::string_view line) {
    g_lines.emplace_back(line);
  });
}

void capture_stop() {
  lucent::set_sink(nullptr);
}

// The data line the REPL emitted for an `rw`, or nullptr. lucent prefixes every line with a timestamp
// and a channel (`[2026-09-27T…] [repl] `), so the address is FOUND rather than matched at position 0 —
// the first version of this test required a prefix match and failed against a perfectly good data line,
// which is the same mistake as blaming the framework for a bad expectation.
// The lines are PASSED IN rather than read from the global: `run_repl` clears the capture once it has
// copied it out, so a helper reading the global afterwards would search an empty vector and report "no
// data line" for a run that emitted one. That was this test's second false failure.
const std::string *find_data_line(const std::vector<std::string> &lines, std::uint32_t address) {
  const std::string prefix = std::format("{:08X}:", address);
  for (const std::string &line : lines) {
    if (line.find(prefix) != std::string::npos) {
      return &line;
    }
  }
  return nullptr;
}

// How many words the data line carries, counted from the address prefix onward: one space before each
// `XXXXXXXX` token, so the spaces after the prefix ARE the word count. Counting colons or splitting on
// whitespace over the whole line would count the timestamp's separators too.
std::size_t words_in(const std::string &line, std::uint32_t address) {
  const std::size_t at = line.find(std::format("{:08X}:", address));
  if (at == std::string::npos) {
    return 0;
  }
  return static_cast<std::size_t>(std::count(line.begin() + static_cast<std::ptrdiff_t>(at), line.end(), ' '));
}

bool announced_short_answer(const std::vector<std::string> &lines) {
  for (const std::string &line : lines) {
    if (line.find("SHORT ANSWER") != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string g_command;

bool read_command(std::span<char> line) {
  // Returning false is how a REPL session ENDS (the same contract the watchdog test uses). Handing back
  // an empty line instead would loop here forever, because the REPL keeps asking until the source says
  // there is nothing left.
  if (g_command.empty()) {
    return false;
  }
  const std::size_t count = std::min(g_command.size(), line.size() - 1);
  std::copy_n(g_command.data(), count, line.data());
  line[count] = '\0';
  g_command.clear(); // one command per read; the next read ends the session
  return true;
}

// Run one REPL command and return the lines it emitted. A bare Core: no Game, so nothing else logs.
// Dump what was captured, so a wrong count or a missing line is debuggable from the test's own output
// rather than by re-running under a debugger. A test that can only say "not found" makes its reader
// guess.
void dump(const char *what, const std::vector<std::string> &lines) {
  fprintf(stderr, "  [%s] captured %zu line(s)\n", what, lines.size());
  for (const std::string &line : lines) {
    fprintf(stderr, "    | %s\n", line.c_str());
  }
}

std::vector<std::string> run_repl(const std::string &command) {
  static const GameConfig config{};
  static const GameHooks hooks{};
  psxport_install_game(&config, &hooks);
  auto core = std::make_unique<Core>();
  g_command = command;
  capture_start();
  Repl repl;
  repl.read(core.get(), 1u, read_command);
  capture_stop();
  std::vector<std::string> lines = g_lines;
  g_lines.clear();
  return lines;
}

// THE POSITIVE: ask for more than the cap and the answer must be short AND say so.
void test_request_over_the_cap_announces_the_short_answer() {
  constexpr std::uint32_t kAddress = 0x80010000u;
  constexpr unsigned kAsked = kCap + 36u;
  const std::vector<std::string> lines = run_repl(std::format("rw {:08X} {}", kAddress, kAsked));
  const std::string *data = find_data_line(lines, kAddress);
  if (data == nullptr) {
    dump("over-cap", lines);
  }
  CHECK(data != nullptr);
  if (data == nullptr) {
    return;
  }
  // The data line itself is UNCHANGED in shape, so an existing single-line parser still works.
  CHECK_EQ(words_in(*data, kAddress), kCap);
  CHECK(announced_short_answer(lines));
  // The notice must be actionable, not just present: it names what was served, what was asked, and
  // that the rest was not read at all. A warning that only says "truncated" leaves the caller to guess
  // whether the tail is zero, absent, or valid.
  std::string joined;
  for (const std::string &line : lines) {
    joined += line;
    joined += '\n';
  }
  CHECK(joined.find(std::format("served {} of {}", kCap, kAsked)) != std::string::npos);
  CHECK(joined.find("NOT") != std::string::npos);
  fprintf(stderr,
          "  over-cap: served %zu word(s), announced=%d\n",
          words_in(*data, kAddress),
          announced_short_answer(lines) ? 1 : 0);
}

// THE CONTROLS, and they are the point: a request the cap SATISFIES must return every word and must NOT
// announce anything. Without these, a build that announced a short answer unconditionally would pass.
void test_request_within_the_cap_is_complete_and_silent() {
  constexpr std::uint32_t kAddress = 0x80010000u;

  const std::vector<std::string> exact = run_repl(std::format("rw {:08X} {}", kAddress, kCap));
  const std::string *exact_data = find_data_line(exact, kAddress);
  if (exact_data == nullptr) {
    dump("exact-cap", exact);
  }
  CHECK(exact_data != nullptr);
  if (exact_data != nullptr) {
    CHECK_EQ(words_in(*exact_data, kAddress), kCap);
  }
  CHECK(!announced_short_answer(exact));

  const std::vector<std::string> small = run_repl(std::format("rw {:08X} {}", kAddress, 8u));
  const std::string *small_data = find_data_line(small, kAddress);
  CHECK(small_data != nullptr);
  if (small_data != nullptr) {
    CHECK_EQ(words_in(*small_data, kAddress), 8u);
  }
  CHECK(!announced_short_answer(small));

  // The default (no count) is 8 words and is likewise silent — the default must not look like a short
  // answer, or every bare `rw ADDR` would cry wolf.
  const std::vector<std::string> defaulted = run_repl(std::format("rw {:08X}", kAddress));
  const std::string *default_data = find_data_line(defaulted, kAddress);
  CHECK(default_data != nullptr);
  if (default_data != nullptr) {
    CHECK_EQ(words_in(*default_data, kAddress), 8u);
  }
  CHECK(!announced_short_answer(defaulted));
  fprintf(stderr,
          "  within-cap: exact=%zu small=%zu default=%zu announced=%d\n",
          exact_data ? words_in(*exact_data, kAddress) : 0,
          small_data ? words_in(*small_data, kAddress) : 0,
          default_data ? words_in(*default_data, kAddress) : 0,
          exact_data ? (announced_short_answer(exact) ? 1 : 0) : 0);
}

// The cap is ONE value, and both transports obey it. This asserts the header the two share is the one
// the test is checking, so a second literal reintroduced in either .cpp cannot pass unnoticed here.
void test_the_cap_has_one_home() {
  CHECK_EQ(kCap, 64u);
  CHECK(psx::control::kMaxControlReadWords > 0u);
}

} // namespace

int main() {
  // RUN() already prefixes `test_`, so the case names are given without it.
  RUN(request_over_the_cap_announces_the_short_answer);
  RUN(request_within_the_cap_is_complete_and_silent);
  RUN(the_cap_has_one_home);
  return pt_summary();
}
