// gpu_perf.h — class GpuPerf — per-frame CPU phase / frame-time profiler (impl gpu_perf.cpp;
// REPL `debug perf`). Owned by Game. Hooks are one cached-int branch when off.
#pragma once
#include "frame_time_histogram.h"

#include <chrono>

class GpuPerf {
public:
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

private:
  using clk = std::chrono::steady_clock;

  // Cached channel state, re-checked lazily: present()/native_step run from BOOT, long before the
  // REPL `debug perf` line is processed, so a one-shot latch would pin it OFF. Re-read every N frames.
  int mPerf = -1; // -1 = unknown, 0/1 cached
  long mPerfRecheck = 0;

  // Every frame's total, kept for the whole run. The rolling average below says what a typical
  // frame costs; only the distribution can say whether any frame missed its budget, which is the
  // question a 60fps product actually has to answer.
  psxport::perf::FrameTimeHistogram mFrames;

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
