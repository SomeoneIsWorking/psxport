// gpu_perf.h — class GpuPerf — the product's cost and pacing meter (impl gpu_perf.cpp; REPL `debug perf`).
// Owned by Game. CPU work per logic frame by phase in a load-independent unit (WorkClock), GPU work per present
// (GpuWork), the planned present cadence, and the wall-clock present series of the play check. The name is stale:
// it meters CPU, GPU and pacing.
#pragma once
#include "count_histogram.h"
#include "frame_time_histogram.h"
#include "gpu_work.h"
#include "work_clock.h"

#include <lucent/log.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace psx::present {
struct PresentNote;
}

class GpuPerf {
public:
  using clk_time_point = std::chrono::steady_clock::time_point;

  // The spans a title's frame driver brackets. A phase name states what the bracketed code IS; rename the phase
  // when the code it brackets changes. Phases nest: a phase's work excludes the phases opened inside it.
  enum class Phase : std::uint8_t {
    PadFence = 0,  // a per-frame input-edge fence, when the title has one
    Audio = 1,     // per-vblank sequencer tick + SPU field advance
    Present = 2,   // the CPU cost of presenting: VRAM upload + record/submit
    GameLogic = 3, // the per-frame game update itself
    Replay = 4,    // ReplayFrame's in-between runs of the title's draw section
  };

  // A phase for the extent of a scope.
  class PhaseScope {
  public:
    PhaseScope(GpuPerf &perf, Phase phase) : perf_(perf), phase_(phase) {
      perf_.phaseBegin(phase_);
    }
    ~PhaseScope() {
      perf_.phaseEnd(phase_);
    }
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;
    PhaseScope(PhaseScope &&) = delete;
    PhaseScope &operator=(PhaseScope &&) = delete;

  private:
    GpuPerf &perf_;
    Phase phase_;
  };

  void frameBegin();            // top of the title FrameDriver: one logic frame begins
  void markPre();               // boundary between pre-tick host work and the guest tick
  void phaseBegin(Phase phase); // open a phase
  void phaseEnd(Phase phase);   // close it, charging its work to the phase
  void frameEnd();              // bottom of the title FrameDriver

  // Whether this frame is metered (the `perf` channel was on at its frameBegin).
  [[nodiscard]] bool counting() const {
    return mPerf > 0;
  }
  // The clock every product thread hands off on; a Coro takes it.
  psx::platform::WorkClock &clock() {
    return mClock;
  }
  // The record rasterization of one present.
  void presentWork(const psx::render::GpuWork &work);

  // The next present holds its picture for `fields` more display fields: the deadline a pacer gave it.
  // Calls before one present add up.
  void heldFor(double fields) {
    mHeldFields += fields;
    mPlanHeldFields += fields;
  }
  // A present was scheduled with `note`, a display field lasting `fieldMs`: the planned cadence takes it.
  void planned(const psx::present::PresentNote &note, double fieldMs);
  // One picture reached the window at `at`. Always on: the play check reads it at run end. It is a miss
  // when it lands over half a period past the fields it was held for, one field when nothing paced it.
  // `vsyncPeriodMs` is the display's refresh period, 0 when unknown.
  void presented(clk_time_point at, double vsyncPeriodMs);
  // Forget the last present, so the next one opens no interval: the gap before a run's first step
  // is boot, not presentation.
  void restartPresentSeries();
  // Run end: the wall-clock present series and the planned cadence on `playcheck`, the cost lines on `perf`.
  void report() const;

  struct PresentSeries {
    std::uint64_t intervals = 0;
    std::uint64_t missed = 0; // intervals over 0.5 periods past their held fields
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
  // A running summary on the `playcheck` channel, for a run that ends without its report.
  static constexpr std::uint64_t kPresentSummaryEvery = 600;
  std::string presentSummary() const;
  std::string cadenceSummary() const;
  void reportCost() const;
  void showOnVblank(std::optional<double> moment);

  // Where a frame's work is charged: the title's phases, the work before markPre, and the rest of the frame.
  enum class Span : std::uint8_t { Pre, PadFence, Audio, GameLogic, Present, Replay, Other, Count };
  static constexpr std::size_t kSpans = static_cast<std::size_t>(Span::Count);
  static Span spanOf(Phase phase);
  void charge();

  // Cached channel state, re-checked lazily: present()/native_step run from BOOT, long before the
  // REPL `debug perf` line is processed, so a one-shot latch would pin it OFF. Re-read every N frames.
  int mPerf = -1; // -1 = unknown, 0/1 cached
  long mPerfRecheck = 0;
  lucent::Channel mFrameChannel{"perfframe"};

  psx::platform::WorkClock mClock;
  std::uint64_t mFrameStart = 0;
  std::uint64_t mLast = 0; // the clock at the last boundary
  std::array<std::uint64_t, kSpans> mFrame{};
  static constexpr std::size_t kMaxDepth = 4;
  std::array<Span, kMaxDepth> mOpen{};
  std::size_t mDepth = 0;
  Span mOutside = Span::Pre; // charged while no phase is open
  std::array<psx::debug::CountHistogram, kSpans> mSpanWork;
  psx::debug::CountHistogram mFrameWork;
  std::uint64_t mMeteredFrames = 0;

  static constexpr std::size_t kGpuCounts = 8;
  std::array<psx::debug::CountHistogram, kGpuCounts> mGpuWork;

  // The planned schedule shown on a simulated 60.00 Hz display: each vblank shows the newest present due by it.
  double mPlanHeldFields = 0.0;
  std::optional<double> mPlanDueMs; // the last planned present's
  std::optional<double> mPlanMoment;
  double mNextVblankMs = 0.0;
  bool mHaveShown = false;
  std::optional<double> mShownMoment;
  std::uint64_t mVblankIndex = 0; // since the plan restarted
  std::uint64_t mVblanks = 0;
  std::uint64_t mPlannedPresents = 0;
  std::uint64_t mUnknownSteps = 0;
  std::map<int, std::uint64_t> mSteps; // hundredths of a logic tick advanced per vblank

  // Present-to-present intervals for the whole run. Only the distribution of what reached the window says
  // whether any frame missed.
  psx::frame::FrameTimeHistogram mFrames;
  PresentSeries mPresents;
  std::uint64_t mMissRun = 0;
  clk_time_point mLastPresent{};
  bool mHavePresent = false;
  double mHeldFields = 0.0; // paced since the last present

  int perfOn();
};
