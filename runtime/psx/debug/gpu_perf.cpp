// gpu_perf.cpp — the cost lines on `perf` (run end) and `perfframe` (per frame and present), the planned cadence and
// the wall-clock present series on `playcheck`.

#include "gpu_perf.h"

#include "display_presenter.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

namespace {

using psx::render::GpuWork;

inline double ms_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

constexpr double kVblankMs = 1000.0 / 60.0; // the simulated display, 60.00 Hz

constexpr std::array<const char *, 7> kSpanNames{
    "pre", "padfence", "audio", "game-logic", "present", "replay", "other"};

struct GpuCount {
  const char *name;
  std::uint64_t GpuWork::*field;
  double scale;
  const char *unit;
};
constexpr std::array<GpuCount, 8> kGpuCountNames{{
    {"draws", &GpuWork::draws, 1.0, "draws"},
    {"pipeline binds", &GpuWork::pipelineBinds, 1.0, "binds"},
    {"render passes", &GpuWork::renderPasses, 1.0, "passes"},
    {"texture copies", &GpuWork::textureCopies, 1.0, "copies"},
    {"triangles", &GpuWork::triangles, 1e-3, "ktri"},
    {"pixels", &GpuWork::pixels, 1e-6, "Mpx"},
    {"copy pixels", &GpuWork::copyPixels, 1e-6, "Mpx"},
    {"upload", &GpuWork::uploadBytes, 1.0 / 1024.0, "KiB"},
}};

// Per-frame work is printed in millions of the clock's unit.
const char *workUnit(psx::platform::WorkUnit unit) {
  return unit == psx::platform::WorkUnit::Instructions ? "Minsn" : "cpu-ms";
}

const char *workSource(psx::platform::WorkUnit unit) {
  return unit == psx::platform::WorkUnit::Instructions
             ? "user-space instructions retired (perf_event, per product thread)"
             : "thread CPU time (no instruction counter on this host), NOT instructions";
}

} // namespace

GpuPerf::Span GpuPerf::spanOf(Phase phase) {
  switch (phase) {
  case Phase::PadFence:
    return Span::PadFence;
  case Phase::Audio:
    return Span::Audio;
  case Phase::Present:
    return Span::Present;
  case Phase::GameLogic:
    return Span::GameLogic;
  case Phase::Replay:
    return Span::Replay;
  }
  return Span::Other;
}

int GpuPerf::perfOn() {
  if (--mPerfRecheck <= 0) {
    mPerf = lucent::channel_on("perf");
    mPerfRecheck = 30;
  }
  return mPerf;
}

void GpuPerf::charge() {
  const std::uint64_t now = mClock.now();
  const Span span = mDepth != 0 ? mOpen[mDepth - 1] : mOutside;
  mFrame[static_cast<std::size_t>(span)] += now - mLast;
  mLast = now;
}

void GpuPerf::frameBegin() {
  if (!perfOn()) {
    mClock.stop();
    return;
  }
  if (mDepth != 0) {
    lucent::error("perf", "a logic frame began inside the {} phase", kSpanNames[static_cast<std::size_t>(mOpen[0])]);
    std::abort();
  }
  mClock.start();
  mFrameStart = mLast = mClock.now();
  mFrame = {};
  mOutside = Span::Pre;
}

void GpuPerf::markPre() {
  if (mPerf <= 0) {
    return;
  }
  charge();
  mOutside = Span::Other;
}

void GpuPerf::phaseBegin(Phase phase) {
  if (mPerf <= 0) {
    return;
  }
  if (mDepth == kMaxDepth) {
    lucent::error("perf", "phases nest deeper than {}", kMaxDepth);
    std::abort();
  }
  charge();
  mOpen[mDepth++] = spanOf(phase);
}

void GpuPerf::phaseEnd(Phase phase) {
  if (mPerf <= 0) {
    return;
  }
  if (mDepth == 0 || mOpen[mDepth - 1] != spanOf(phase)) {
    lucent::error("perf",
                  "the {} phase ended while it was not the innermost open phase",
                  kSpanNames[static_cast<std::size_t>(spanOf(phase))]);
    std::abort();
  }
  charge();
  --mDepth;
}

void GpuPerf::frameEnd() {
  if (mPerf <= 0) {
    return;
  }
  charge();
  if (mDepth != 0) {
    lucent::error("perf", "a logic frame ended inside the {} phase", kSpanNames[static_cast<std::size_t>(mOpen[0])]);
    std::abort();
  }
  for (std::size_t span = 0; span < kSpans; ++span) {
    mSpanWork[span].add(mFrame[span]);
  }
  const std::uint64_t total = mLast - mFrameStart;
  mFrameWork.add(total);
  ++mMeteredFrames;
  lucent::debug(mFrameChannel,
                "frame {} {}: pre={} padfence={} audio={} game-logic={} present={} replay={} other={} total={}",
                mMeteredFrames,
                mClock.unit() == psx::platform::WorkUnit::Instructions ? "insn" : "cpu-ns",
                mFrame[0],
                mFrame[1],
                mFrame[2],
                mFrame[3],
                mFrame[4],
                mFrame[5],
                mFrame[6],
                total);
}

void GpuPerf::presentWork(const psx::render::GpuWork &work) {
  if (mPerf <= 0) {
    return;
  }
  for (std::size_t count = 0; count < kGpuCounts; ++count) {
    mGpuWork[count].add(work.*kGpuCountNames[count].field);
  }
  lucent::debug(mFrameChannel,
                "present gpu: draws={} binds={} passes={} copies={} triangles={} pixels={} copy-pixels={} "
                "upload-bytes={}",
                work.draws,
                work.pipelineBinds,
                work.renderPasses,
                work.textureCopies,
                work.triangles,
                work.pixels,
                work.copyPixels,
                work.uploadBytes);
}

void GpuPerf::planned(const psx::present::PresentNote &note, double fieldMs) {
  ++mPlannedPresents;
  std::optional<double> moment;
  if (note.shown) {
    const auto shown = static_cast<double>(*note.shown);
    moment = note.from ? static_cast<double>(*note.from) + note.t * (shown - static_cast<double>(*note.from)) : shown;
  }
  const double heldMs = mPlanHeldFields * fieldMs;
  mPlanHeldFields = 0.0;
  if (!mPlanDueMs) {
    mPlanDueMs = 0.0;
    mVblankIndex = 0;
    mNextVblankMs = 0.0;
    mPlanMoment = moment;
    return;
  }
  const double due = *mPlanDueMs + heldMs;
  while (mNextVblankMs < due) {
    showOnVblank(mPlanMoment);
    mNextVblankMs = static_cast<double>(++mVblankIndex) * kVblankMs;
  }
  mPlanDueMs = due;
  mPlanMoment = moment;
}

void GpuPerf::showOnVblank(std::optional<double> moment) {
  ++mVblanks;
  if (mHaveShown && moment && mShownMoment) {
    ++mSteps[static_cast<int>(std::lround((*moment - *mShownMoment) * 100.0))];
  } else if (mHaveShown) {
    ++mUnknownSteps;
  }
  mHaveShown = true;
  mShownMoment = moment;
  if (mVblanks % kPresentSummaryEvery == 0) {
    lucent::debug("playcheck", "cadence so far: {}", cadenceSummary());
  }
}

void GpuPerf::restartPresentSeries() {
  mHavePresent = false;
  mHeldFields = 0.0;
  mPlanDueMs.reset();
  mPlanHeldFields = 0.0;
  mHaveShown = false;
  mVblankIndex = 0;
}

void GpuPerf::presented(clk_time_point at, double vsyncPeriodMs) {
  const double heldFields = mHeldFields > 0.0 ? mHeldFields : 1.0;
  mHeldFields = 0.0;
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
  if (mPresents.vsyncPeriodMs > 0.0 && interval > (heldFields + 0.5) * mPresents.vsyncPeriodMs) {
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

void GpuPerf::report() const {
  lucent::info("playcheck", "presents: {}", presentSummary());
  lucent::info("playcheck", "cadence: {}", cadenceSummary());
  reportCost();
}

void GpuPerf::reportCost() const {
  const psx::platform::WorkUnit unit = mClock.unit();
  const char *unitName = workUnit(unit);
  lucent::debug("perf",
                "cpu run-end: {} logic frames metered in {}; {} product thread(s) uncounted",
                mMeteredFrames,
                workSource(unit),
                mClock.uncountedThreads());
  const auto line = [&](const char *name, const psx::debug::CountHistogram &work) {
    lucent::debug("perf",
                  "cpu {}: p50 {:.3f} p99 {:.3f} max {:.3f} {}/frame, total {:.1f} {}",
                  name,
                  static_cast<double>(work.percentile(0.50)) * 1e-6,
                  static_cast<double>(work.percentile(0.99)) * 1e-6,
                  static_cast<double>(work.max()) * 1e-6,
                  unitName,
                  static_cast<double>(work.total()) * 1e-6,
                  unitName);
  };
  for (std::size_t span = 0; span < kSpans; ++span) {
    line(kSpanNames[span], mSpanWork[span]);
  }
  line("frame", mFrameWork);
  lucent::debug("perf", "gpu run-end: {} record presents metered", mGpuWork[0].count());
  for (std::size_t count = 0; count < kGpuCounts; ++count) {
    const GpuCount &named = kGpuCountNames[count];
    const psx::debug::CountHistogram &work = mGpuWork[count];
    lucent::debug("perf",
                  "gpu {}: p50 {:.3f} p99 {:.3f} max {:.3f} {}/present, total {:.1f} {}",
                  named.name,
                  static_cast<double>(work.percentile(0.50)) * named.scale,
                  static_cast<double>(work.percentile(0.99)) * named.scale,
                  static_cast<double>(work.max()) * named.scale,
                  named.unit,
                  static_cast<double>(work.total()) * named.scale,
                  named.unit);
  }
}

std::string GpuPerf::cadenceSummary() const {
  std::string steps;
  for (const auto &[hundredths, vblanks] : mSteps) {
    steps += std::format(" {:.2f}x{}", static_cast<double>(hundredths) / 100.0, vblanks);
  }
  return std::format("vblanks={} planned_presents={} unknown={} logic ticks advanced per 60.00 Hz vblank:{}",
                     mVblanks,
                     mPlannedPresents,
                     mUnknownSteps,
                     steps);
}

std::string GpuPerf::presentSummary() const {
  const double missedPct =
      mPresents.intervals != 0 ? 100.0 * (double)mPresents.missed / (double)mPresents.intervals : 0.0;
  return std::format("intervals={} p50={:.2f}ms p99={:.2f}ms worst={:.2f}ms vsync={:.2f}ms "
                     "missed={} ({:.2f}%) longest_miss_run={} beyond_range={}",
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
