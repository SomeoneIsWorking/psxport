// gpu_perf.h — class GpuPerf — per-frame CPU phase / frame-time profiler (impl gpu_perf.cpp;
// REPL `debug perf`). Owned by Game. Hooks are one cached-int branch when off.
#pragma once
#include "frame_time_histogram.h"

#include <chrono>
#include <cstdint>
#include <string>

class GpuPerf {
public:
  using clk_time_point = std::chrono::steady_clock::time_point;

  // The spans a title's frame driver brackets. A phase name states what the bracketed code IS, so a
  // 0.00 ms reading means "this work is cheap" and never "the work moved somewhere this name still
  // claims to cover" — see the banner in gpu_perf.cpp for the session that misread exactly that.
  // Rename the phase when the code it brackets changes; do not leave a stale label behind.
  enum class Phase : int {
    PadFence = 0,  // a per-frame input-edge fence, when the title has one
    Audio = 1,     // per-vblank sequencer tick + SPU field advance
    Present = 2,   // the CPU cost of presenting: VRAM upload + record/submit
    GameLogic = 3, // the per-frame game update itself
  };

  void frameBegin();            // top of the title FrameDriver: start the frame clock
  void markPre();               // boundary between pre-tick host work and the guest tick
  void phaseBegin(Phase phase); // open a timed phase
  void phaseEnd(Phase phase);   // close it, charging elapsed time to the phase slot
  void frameEnd();              // bottom of the title FrameDriver: close frame + rolling avg

  // One picture reached the window at `at`. Always on: the play check reads it at run end.
  // `vsyncPeriodMs` is the display's refresh period, 0 when unknown.
  void presented(clk_time_point at, double vsyncPeriodMs);
  // Forget the last present, so the next one opens no interval: the gap before a run's first step
  // is boot, not presentation.
  void restartPresentSeries() {
    mHavePresent = false;
  }
  // The present-to-present series, once, at run end.
  void reportPresents() const;

  struct PresentSeries {
    std::uint64_t intervals = 0;
    std::uint64_t missed = 0; // intervals over 1.5 vsync periods
    std::uint64_t longestMissRun = 0;
    double vsyncPeriodMs = 0.0;
  };
  PresentSeries presentSeries() const {
    return mPresents;
  }
  const psx::frame::FrameTimeHistogram &presentIntervals() const {
    return mFrames;
  }

private:
  using clk = std::chrono::steady_clock;
  // A running summary on the `playcheck` channel, for a run that ends without its report.
  static constexpr std::uint64_t kPresentSummaryEvery = 600;
  std::string presentSummary() const;

  // Cached channel state, re-checked lazily: present()/native_step run from BOOT, long before the
  // REPL `debug perf` line is processed, so a one-shot latch would pin it OFF. Re-read every N frames.
  int mPerf = -1; // -1 = unknown, 0/1 cached
  long mPerfRecheck = 0;

  // Present-to-present intervals for the whole run. The rolling average below says what a typical
  // frame costs; only the distribution of what reached the window says whether any frame missed.
  psx::frame::FrameTimeHistogram mFrames;
  PresentSeries mPresents;
  std::uint64_t mMissRun = 0;
  clk_time_point mLastPresent{};
  bool mHavePresent = false;

  // Phase accumulators (ms), summed across the averaging window, reset every report. One field per
  // Phase, named identically, so the report label and the slot it prints cannot drift apart.
  struct Acc {
    double frame = 0, pre = 0, padFence = 0, audio = 0, present = 0, gameLogic = 0, post = 0;
    long frames = 0;
  } mAcc;

  // Per-frame scratch timestamps.
  clk::time_point mTFrame; // frame begin
  clk::time_point mTPhase; // current open phase begin
  clk::time_point mTMark;  // last "mark" boundary (for pre/post measurement)

  double *phaseSlot(Phase phase);
  int perfOn();
};
