// gpu_perf.cpp — PORTABLE per-frame CPU phase / frame-time profiler for the native port.
// ----------------------------------------------------------------------------------------------------
// Companion to the `vkprof` channel in gpu_vk.cpp (which times the GPU present side: VRAM upload, the
// VK render pass via timestamp queries, and prim counts). This module fills the gap vkprof can't see:
// the per-frame CPU phase breakdown of the GUEST-LOGIC half of the frame, plus the overall
// present-to-present frame time, so "CPU-bound vs GPU-bound" is answerable directly.
//
// THE FRAME, as the native loop runs it (native_boot.cpp native_step_frame -> ov_frame_update):
//   FRAME boundary  -- perf.frameBegin()  (top of native_step_frame)
//     [pre-tick host work: input, IRQ events, OT clear]
//     LOGIC+SUBMIT   = rec_super_call(0x800788AC): ALL guest interpreter work + render-command submit.
//                      Bracketed by perf.phaseBegin/End(LOGIC) in ov_frame_update around the super-call.
//     AUDIO          = the per-vblank sequencer tick + SPU field advance (ov_frame_update).
//     PRESENT        = gpu_present(): VRAM mirror upload + VK record/submit (the CPU cost of present;
//                      the GPU-side ms is what vkprof's timestamp query reports separately).
//     [post-tick host work: scheduler step, draw sync, OT submit]
//   FRAME boundary  -- perf.frameEnd()    (bottom of native_step_frame): wall delta = full frame time.
//
// All timing is std::chrono::steady_clock (monotonic, portable to macOS/MoltenVK — NOT gettimeofday /
// wall-clock-of-day). Default OFF: enabled at runtime via the REPL `debug perf` channel (or
// PSXPORT_DEBUG=perf). When OFF the hooks cost one cached-int branch each — no clock reads, no overhead.
// Prints a rolling average every 60 frames to stderr, e.g.:
//
//   [perf] 60f avg 8.42ms (118.8 fps) | frame 8.42 = pre 0.10 LOGIC 6.80 audio 0.40 PRESENT-cpu 0.18 post 0.05 + idle/pace 0.89 | <-CPU sum 7.53ms
//
// Read it against vkprof's "GPU X.XXms": if the CPU phase sum ~= frame time and GPU ms << frame time,
// the port is CPU-BOUND (the interpreter LOGIC phase is the lever — own more hot guest fns native). If
// GPU ms ~= frame time while CPU phases are small, it is GPU-BOUND (the present/raster path is the lever).

#include "gpu_perf.h"
#include "cfg.h"
#include <cstdio>

static inline double ms_between(std::chrono::steady_clock::time_point a,
                                std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

enum Phase { PH_LOGIC = 0, PH_AUDIO = 1, PH_PRESENT = 2, PH_SCHED = 3 };
double* GpuPerf::phaseSlot(int p) {
  switch (p) { case PH_LOGIC: return &mAcc.logic; case PH_AUDIO: return &mAcc.audio;
               case PH_PRESENT: return &mAcc.present; case PH_SCHED: return &mAcc.sched;
               default: return nullptr; }
}

int GpuPerf::perfOn() {
  if (--mPerfRecheck <= 0) { mPerf = cfg_dbg("perf"); mPerfRecheck = 30; }
  return mPerf;
}

// Top of native_step_frame: start the frame clock.
void GpuPerf::frameBegin() {
  if (!perfOn()) return;
  mTFrame = mTMark = clk::now();
}

// Mark the boundary between the pre-tick host work and the next phase (charges elapsed since the last
// mark to `pre`). Called just before the guest tick begins.
void GpuPerf::markPre() {
  if (mPerf <= 0) return;
  clk::time_point n = clk::now();
  mAcc.pre += ms_between(mTMark, n);
  mTMark = n;
}

// Open a timed phase (LOGIC / AUDIO / PRESENT).
void GpuPerf::phaseBegin(int phase) {
  if (mPerf <= 0) return;
  (void)phase;
  mTPhase = clk::now();
}

// Close the open phase, charging its elapsed time to the phase slot and advancing the post-mark.
void GpuPerf::phaseEnd(int phase) {
  if (mPerf <= 0) return;
  clk::time_point n = clk::now();
  double* s = phaseSlot(phase);
  if (s) *s += ms_between(mTPhase, n);
  mTMark = n;   // anything after the last closed phase counts toward `post`
}

// Bottom of native_step_frame: close the frame, accumulate the post-tick remainder + the full frame
// wall time, and emit the rolling average every 60 frames.
void GpuPerf::frameEnd() {
  if (mPerf <= 0) return;
  clk::time_point n = clk::now();
  mAcc.post  += ms_between(mTMark, n);
  mAcc.frame += ms_between(mTFrame, n);
  if (++mAcc.frames < 60) return;

  double nf = (double)mAcc.frames;
  double frame = mAcc.frame / nf;
  double pre = mAcc.pre / nf, logic = mAcc.logic / nf, audio = mAcc.audio / nf;
  double present = mAcc.present / nf, sched = mAcc.sched / nf, post = mAcc.post / nf;
  double cpu_sum = pre + logic + audio + present + sched + post;
  double idle = frame - cpu_sum;   // pacing / vsync sleep / anything outside the measured spans
  fprintf(stderr,
    "[perf] %.0ff avg %.2fms (%.1f fps) | frame %.2f = pre %.2f tick-LOGIC %.2f audio %.2f "
    "PRESENT-cpu %.2f SCHED-LOGIC %.2f post %.2f + idle/pace %.2f | CPU-sum %.2fms\n",
    nf, frame, frame > 0 ? 1000.0 / frame : 0.0,
    frame, pre, logic, audio, present, sched, post, idle, cpu_sum);

  mAcc = Acc{};
}
