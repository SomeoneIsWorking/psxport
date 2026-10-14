// gpu_perf.cpp — PORTABLE per-frame CPU phase / frame-time profiler for the native port.
// ----------------------------------------------------------------------------------------------------
// Companion to the `vkprof` channel in gpu_vk.cpp (which times the GPU present side: VRAM upload, the
// VK render pass via timestamp queries, and prim counts). This module fills the gap vkprof can't see:
// the per-frame CPU phase breakdown of the GUEST-LOGIC half of the frame, plus the overall
// present-to-present frame time, so "CPU-bound vs GPU-bound" is answerable directly.
//
// THE FRAME, as the title's native FrameDriver runs it:
//   FRAME boundary  -- perf.frameBegin()  (top of FrameDriver::stepFrame)
//     [pre-tick host work: input, IRQ events, OT clear]
//     Phase::PadFence / Audio / Present / GameLogic, whichever the title brackets. gpu_perf.h
//     defines what each one measures; a title opens only the phases it actually has.
//
//     PadFence times only the title's input-edge fence, not the frame's guest work.
//     [post-tick host work: draw sync, OT submit]
//   FRAME boundary  -- perf.frameEnd()    (bottom of FrameDriver::stepFrame): wall delta = full frame time.
//
// All timing is std::chrono::steady_clock (monotonic, portable to macOS/MoltenVK — NOT gettimeofday /
// wall-clock-of-day). Default OFF: enabled at runtime via the REPL `debug perf` channel (or
// PSXPORT_DEBUG=perf). When OFF the hooks cost one cached-int branch each — no clock reads, no overhead.
// Emits a rolling average every 60 frames through the configured logger, e.g.:
//
//   [perf] 60f avg 4.42ms (226.2 fps) | frame 4.42 = pre 0.02 padfence 0.00 audio 0.30 present-cpu 2.42
//   idle/pace 0.89 | <-CPU sum 7.53ms
//
// Read it against vkprof's "GPU X.XXms": if the CPU phase sum ~= frame time and GPU ms << frame time,
// the port is CPU-BOUND. The lever is whichever phase actually holds the time — read the line, do not
// GPU ms ~= frame time while CPU phases are small, it is GPU-BOUND (the present/raster path is the lever).

#include "gpu_perf.h"
#include <cstdio>
#include <format>
#include <lucent/log.h>
#include <string>

static inline double ms_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// One arm per Phase, each returning the identically named slot. `Phase` is exhaustive, so a new
// phase without a slot is a compile error rather than a silently dropped measurement.
double *GpuPerf::phaseSlot(Phase phase) {
  switch (phase) {
  case Phase::PadFence:
    return &mAcc.padFence;
  case Phase::Audio:
    return &mAcc.audio;
  case Phase::Present:
    return &mAcc.present;
  case Phase::GameLogic:
    return &mAcc.gameLogic;
  }
  return nullptr;
}

int GpuPerf::perfOn() {
  if (--mPerfRecheck <= 0) {
    mPerf = lucent::channel_on("perf");
    mPerfRecheck = 30;
  }
  return mPerf;
}

// Top of the title-owned frame step: start the frame clock.
void GpuPerf::frameBegin() {
  if (!perfOn()) {
    return;
  }
  mTFrame = mTMark = clk::now();
}

// Mark the boundary between the pre-tick host work and the next phase (charges elapsed since the last
// mark to `pre`). Called just before the guest tick begins.
void GpuPerf::markPre() {
  if (mPerf <= 0) {
    return;
  }
  clk::time_point n = clk::now();
  mAcc.pre += ms_between(mTMark, n);
  mTMark = n;
}

// Open a timed phase.
void GpuPerf::phaseBegin(Phase phase) {
  if (mPerf <= 0) {
    return;
  }
  (void)phase;
  mTPhase = clk::now();
}

// Close the open phase, charging its elapsed time to the phase slot and advancing the post-mark.
void GpuPerf::phaseEnd(Phase phase) {
  if (mPerf <= 0) {
    return;
  }
  clk::time_point n = clk::now();
  double *s = phaseSlot(phase);
  if (s) {
    *s += ms_between(mTPhase, n);
  }
  mTMark = n; // anything after the last closed phase counts toward `post`
}

// Bottom of the title-owned frame step: close the frame, accumulate the post-tick remainder + the full frame
// wall time, and emit the rolling average every 60 frames.
void GpuPerf::frameEnd() {
  if (mPerf <= 0) {
    return;
  }
  clk::time_point n = clk::now();
  mAcc.post += ms_between(mTMark, n);
  const double thisFrame = ms_between(mTFrame, n);
  mAcc.frame += thisFrame;
  if (++mAcc.frames < 60) {
    return;
  }

  double nf = (double)mAcc.frames;
  double frame = mAcc.frame / nf;
  double pre = mAcc.pre / nf, padFence = mAcc.padFence / nf, audio = mAcc.audio / nf;
  double present = mAcc.present / nf, gameLogic = mAcc.gameLogic / nf, post = mAcc.post / nf;
  double cpu_sum = pre + padFence + audio + present + gameLogic + post;
  double idle = frame - cpu_sum; // pacing / vsync sleep / anything outside the measured spans
  lucent::info("perf",
               "{:.0f}f avg {:.2f}ms ({:.1f} fps) | frame {:.2f} = pre {:.2f} padfence {:.2f} audio {:.2f} "
               "present-cpu {:.2f} game-logic {:.2f} post {:.2f} + idle/pace {:.2f} | CPU-sum {:.2f}ms",
               nf,
               frame,
               frame > 0 ? 1000.0 / frame : 0.0,
               frame,
               pre,
               padFence,
               audio,
               present,
               gameLogic,
               post,
               idle,
               cpu_sum);
  // The average above cannot answer a budget: one late present in a thousand is a visible stutter
  // and does not move a mean. Cumulative over the whole run; `beyond-range` is not folded into the
  // last bucket.
  lucent::info("perf",
               "{} present distribution p50 {:.2f} p95 {:.2f} p99 {:.2f} worst {:.2f}ms beyond-range {} "
               "over-1.5-vsync {} longest-miss-run {}",
               mFrames.count(),
               mFrames.percentileMs(0.50),
               mFrames.percentileMs(0.95),
               mFrames.percentileMs(0.99),
               mFrames.worstMs(),
               mFrames.beyondRange(),
               mPresents.missed,
               mPresents.longestMissRun);

  mAcc = Acc{};
}

void GpuPerf::presented(clk_time_point at, double vsyncPeriodMs) {
  if (!mHavePresent) {
    mHavePresent = true;
    mLastPresent = at;
    return;
  }
  const double interval = ms_between(mLastPresent, at);
  mLastPresent = at;
  mFrames.add(interval);
  ++mPresents.intervals;
  if (vsyncPeriodMs > 0.0) {
    mPresents.vsyncPeriodMs = vsyncPeriodMs;
  }
  if (mPresents.vsyncPeriodMs > 0.0 && interval > 1.5 * mPresents.vsyncPeriodMs) {
    ++mPresents.missed;
    ++mMissRun;
    mPresents.longestMissRun = mMissRun > mPresents.longestMissRun ? mMissRun : mPresents.longestMissRun;
  } else {
    mMissRun = 0;
  }
  if (mPresents.intervals % kPresentSummaryEvery == 0) {
    lucent::debug("playcheck", "presents so far: {}", presentSummary());
  }
}

void GpuPerf::reportPresents() const {
  lucent::info("playcheck", "presents: {}", presentSummary());
}

std::string GpuPerf::presentSummary() const {
  const double missedPct =
      mPresents.intervals != 0 ? 100.0 * (double)mPresents.missed / (double)mPresents.intervals : 0.0;
  return std::format("intervals={} p50={:.2f}ms p99={:.2f}ms worst={:.2f}ms vsync={:.2f}ms "
                     "over_1.5_vsync={} ({:.2f}%) longest_miss_run={} beyond_range={}",
                     mPresents.intervals,
                     mFrames.percentileMs(0.50),
                     mFrames.percentileMs(0.99),
                     mFrames.worstMs(),
                     mPresents.vsyncPeriodMs,
                     mPresents.missed,
                     missedPct,
                     mPresents.longestMissRun,
                     mFrames.beyondRange());
}
