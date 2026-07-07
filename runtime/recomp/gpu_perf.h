// gpu_perf.h — class GpuPerf — per-frame CPU phase / frame-time profiler (impl gpu_perf.cpp;
// REPL `debug perf`). Owned by Game. Phases: 0 = LOGIC, 1 = AUDIO, 2 = PRESENT, 3 = SCHED.
// Hooks are one cached-int branch when off.
#pragma once
#include <chrono>

class GpuPerf {
public:
  void frameBegin();       // top of native_step_frame: start the frame clock
  void markPre();          // boundary between pre-tick host work and the guest tick
  void phaseBegin(int phase);  // open a timed phase
  void phaseEnd(int phase);    // close it, charging elapsed time to the phase slot
  void frameEnd();         // bottom of native_step_frame: close frame + rolling avg

private:
  using clk = std::chrono::steady_clock;

  // Cached channel state, re-checked lazily: present()/native_step run from BOOT, long before the
  // REPL `debug perf` line is processed, so a one-shot latch would pin it OFF. Re-read every N frames.
  int  mPerf = -1;             // -1 = unknown, 0/1 cached
  long mPerfRecheck = 0;

  // Phase accumulators (ms), summed across the averaging window, reset every report.
  struct Acc {
    double frame = 0, pre = 0, logic = 0, audio = 0, present = 0, sched = 0, post = 0;
    long   frames = 0;
  } mAcc;

  // Per-frame scratch timestamps.
  clk::time_point mTFrame;     // frame begin
  clk::time_point mTPhase;     // current open phase begin (logic/audio/present)
  clk::time_point mTMark;      // last "mark" boundary (for pre/post measurement)

  double* phaseSlot(int p);
  int     perfOn();
};
