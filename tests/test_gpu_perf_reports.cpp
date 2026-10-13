// test_gpu_perf_reports.cpp — the profiler's own falsifier.
//
// GpuPerf is the only thing that answers "did any frame miss its budget", and it answers by LOGGING.
// Every one of its hooks is an early `return` when the channel is off, so a wiring mistake makes it
// print nothing — which is exactly what an off channel prints, and what a driver that never calls
// frameEnd() prints. Nothing distinguished those three until this test. It drives the shipping class
// through one full reporting window and requires the line, its phase labels, and its distribution.
#include "gpu_perf.h"
#include "testutil.h"

#include <cstdlib>
#include <lucent/log.h>
#include <string>
#include <vector>

namespace {

// One reporting window of frames, each opening the two phases a title actually brackets.
void driveFrames(GpuPerf &perf, int frames) {
  for (int i = 0; i < frames; ++i) {
    perf.frameBegin();
    perf.markPre();
    perf.phaseBegin(GpuPerf::Phase::GameLogic);
    perf.phaseEnd(GpuPerf::Phase::GameLogic);
    perf.phaseBegin(GpuPerf::Phase::Present);
    perf.phaseEnd(GpuPerf::Phase::Present);
    perf.frameEnd();
  }
}

std::vector<std::string> capture(int frames) {
  std::vector<std::string> lines;
  lucent::set_sink([&lines](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });
  GpuPerf perf;
  driveFrames(perf, frames);
  lucent::set_sink(nullptr);
  return lines;
}

bool anyContains(const std::vector<std::string> &lines, const char *needle) {
  for (const std::string &line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

// ── the positive: the channel is on and a full window closes ────────────────────────────────────
static void test_a_closed_window_reports_its_average_and_its_distribution(void) {
  lucent::enable_channels("perf");
  const std::vector<std::string> lines = capture(60);
  CHECK(!lines.empty());
  CHECK(anyContains(lines, "[perf]"));
  CHECK(anyContains(lines, "60f avg"));
  CHECK(anyContains(lines, "distribution p50"));
  CHECK(anyContains(lines, "beyond-range"));
}

// ── the phase labels name the slots the title opened ────────────────────────────────────────────
static void test_the_average_line_names_every_phase(void) {
  lucent::enable_channels("perf");
  const std::vector<std::string> lines = capture(60);
  CHECK(anyContains(lines, "pre "));
  CHECK(anyContains(lines, "padfence "));
  CHECK(anyContains(lines, "audio "));
  CHECK(anyContains(lines, "present-cpu "));
  CHECK(anyContains(lines, "game-logic "));
  CHECK(anyContains(lines, "post "));
}

// ── the negative: a window that has not closed yet says nothing ─────────────────────────────────
static void test_an_unfinished_window_reports_nothing(void) {
  lucent::enable_channels("perf");
  const std::vector<std::string> lines = capture(59);
  CHECK_EQ((int)lines.size(), 0);
}

// ── the other negative: the channel is off ──────────────────────────────────────────────────────
static void test_an_off_channel_reports_nothing(void) {
  lucent::enable_channels("");
  const std::vector<std::string> lines = capture(120);
  CHECK_EQ((int)lines.size(), 0);
}

// ── the window repeats for as long as the run does ──────────────────────────────────────────────
static void test_two_windows_report_twice(void) {
  lucent::enable_channels("perf");
  const std::vector<std::string> lines = capture(120);
  int averages = 0;
  for (const std::string &line : lines) {
    if (line.find("60f avg") != std::string::npos) {
      ++averages;
    }
  }
  CHECK_EQ(averages, 2);
}

// ── the present series is recorded with the channel off, and counts misses and their longest run ──
static void test_presents_count_misses_and_the_longest_run(void) {
  lucent::enable_channels("");
  GpuPerf perf;
  const double vsync = 1000.0 / 60.0;
  const std::chrono::steady_clock::time_point start{};
  double at = 0.0;
  // 16.7, 40, 40, 16.7, 30 ms: three intervals over 1.5 vsync, two of them in a row.
  for (double interval : {0.0, 16.7, 40.0, 40.0, 16.7, 30.0}) {
    at += interval;
    perf.presented(start + std::chrono::microseconds(static_cast<long>(at * 1000.0)), vsync);
  }
  const GpuPerf::PresentSeries series = perf.presentSeries();
  CHECK_EQ(series.intervals, 5u);
  CHECK_EQ(series.missed, 3u);
  CHECK_EQ(series.longestMissRun, 2u);
  CHECK(perf.presentIntervals().worstMs() >= 39.9);
}

// ── a restart drops the gap before it: boot is not a missed present ──
static void test_a_restarted_series_opens_no_interval_across_the_gap(void) {
  lucent::enable_channels("");
  GpuPerf perf;
  const double vsync = 1000.0 / 60.0;
  const std::chrono::steady_clock::time_point start{};
  perf.presented(start, vsync);
  perf.restartPresentSeries();
  perf.presented(start + std::chrono::milliseconds(7500), vsync);
  perf.presented(start + std::chrono::microseconds(7516700), vsync);
  const GpuPerf::PresentSeries series = perf.presentSeries();
  CHECK_EQ(series.intervals, 1u);
  CHECK_EQ(series.missed, 0u);
  CHECK(perf.presentIntervals().worstMs() < 17.0);
}

int main(void) {
  RUN(presents_count_misses_and_the_longest_run);
  RUN(a_restarted_series_opens_no_interval_across_the_gap);
  RUN(a_closed_window_reports_its_average_and_its_distribution);
  RUN(the_average_line_names_every_phase);
  RUN(an_unfinished_window_reports_nothing);
  RUN(an_off_channel_reports_nothing);
  RUN(two_windows_report_twice);
  return pt_summary();
}
