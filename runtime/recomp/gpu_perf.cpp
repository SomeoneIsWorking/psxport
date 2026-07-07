// gpu_perf.cpp — PORTABLE per-frame CPU phase / frame-time profiler for the native port.
// ----------------------------------------------------------------------------------------------------
// Companion to the `vkprof` channel in gpu_gpu.cpp (which times the GPU present side: VRAM upload, the
// VK render pass via timestamp queries, and prim counts). This module fills the gap vkprof can't see:
// the per-frame CPU phase breakdown of the GUEST-LOGIC half of the frame, plus the overall
// present-to-present frame time, so "CPU-bound vs GPU-bound" is answerable directly.
//
// THE FRAME, as the native loop runs it (native_boot.cpp native_step_frame -> ov_frame_update):
//   FRAME boundary  -- perf_frame_begin()  (top of native_step_frame)
//     [pre-tick host work: input, IRQ events, OT clear]
//     LOGIC+SUBMIT   = rec_super_call(0x800788AC): ALL guest interpreter work + render-command submit.
//                      Bracketed by perf_phase(LOGIC) begin/end in ov_frame_update around the super-call.
//     AUDIO          = the per-vblank sequencer tick + SPU field advance (ov_frame_update).
//     PRESENT        = gpu_present(): VRAM mirror upload + VK record/submit (the CPU cost of present;
//                      the GPU-side ms is what vkprof's timestamp query reports separately).
//     [post-tick host work: scheduler step, draw sync, OT submit]
//   FRAME boundary  -- perf_frame_end()    (bottom of native_step_frame): wall delta = full frame time.
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

#include "cfg.h"
#include <chrono>
#include <cstdio>

namespace {

using clk = std::chrono::steady_clock;
static inline double ms_between(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// Cached channel state, re-checked lazily: present()/native_step run from BOOT, long before the REPL
// `debug perf` line is processed, so a one-shot static latch would pin it OFF. Re-read every N frames.
static int s_perf = -1;         // -1 = unknown, 0/1 cached (was global g_perf)
static long s_perf_recheck = 0;

// Phase accumulators (ms), summed across the averaging window, reset every report.
struct Acc {
  double frame = 0, pre = 0, logic = 0, audio = 0, present = 0, sched = 0, post = 0;
  long   frames = 0;
} g_acc;

// Per-frame scratch timestamps.
clk::time_point g_t_frame;      // frame begin
clk::time_point g_t_phase;      // current open phase begin (logic/audio/present)
clk::time_point g_t_mark;       // last "mark" boundary (for pre/post measurement)

enum Phase { PH_LOGIC = 0, PH_AUDIO = 1, PH_PRESENT = 2, PH_SCHED = 3 };
double* phase_slot(int p) {
  switch (p) { case PH_LOGIC: return &g_acc.logic; case PH_AUDIO: return &g_acc.audio;
               case PH_PRESENT: return &g_acc.present; case PH_SCHED: return &g_acc.sched;
               default: return nullptr; }
}

inline int perf_on() {
  if (--s_perf_recheck <= 0) { s_perf = cfg_dbg("perf"); s_perf_recheck = 30; }
  return s_perf;
}

} // namespace

#include "gpu_perf.h"

// Top of native_step_frame: start the frame clock.
void perf_frame_begin(void) {
  if (!perf_on()) return;
  g_t_frame = g_t_mark = clk::now();
}

// Mark the boundary between the pre-tick host work and the next phase (charges elapsed since the last
// mark to `pre`). Called just before the guest tick begins.
void perf_mark_pre(void) {
  if (s_perf <= 0) return;
  clk::time_point n = clk::now();
  g_acc.pre += ms_between(g_t_mark, n);
  g_t_mark = n;
}

// Open a timed phase (LOGIC / AUDIO / PRESENT).
void perf_phase_begin(int phase) {
  if (s_perf <= 0) return;
  (void)phase;
  g_t_phase = clk::now();
}

// Close the open phase, charging its elapsed time to the phase slot and advancing the post-mark.
void perf_phase_end(int phase) {
  if (s_perf <= 0) return;
  clk::time_point n = clk::now();
  double* s = phase_slot(phase);
  if (s) *s += ms_between(g_t_phase, n);
  g_t_mark = n;   // anything after the last closed phase counts toward `post`
}

// Bottom of native_step_frame: close the frame, accumulate the post-tick remainder + the full frame
// wall time, and emit the rolling average every 60 frames.
void perf_frame_end(void) {
  if (s_perf <= 0) return;
  clk::time_point n = clk::now();
  g_acc.post  += ms_between(g_t_mark, n);
  g_acc.frame += ms_between(g_t_frame, n);
  if (++g_acc.frames < 60) return;

  double nf = (double)g_acc.frames;
  double frame = g_acc.frame / nf;
  double pre = g_acc.pre / nf, logic = g_acc.logic / nf, audio = g_acc.audio / nf;
  double present = g_acc.present / nf, sched = g_acc.sched / nf, post = g_acc.post / nf;
  double cpu_sum = pre + logic + audio + present + sched + post;
  double idle = frame - cpu_sum;   // pacing / vsync sleep / anything outside the measured spans
  fprintf(stderr,
    "[perf] %.0ff avg %.2fms (%.1f fps) | frame %.2f = pre %.2f tick-LOGIC %.2f audio %.2f "
    "PRESENT-cpu %.2f SCHED-LOGIC %.2f post %.2f + idle/pace %.2f | CPU-sum %.2fms\n",
    nf, frame, frame > 0 ? 1000.0 / frame : 0.0,
    frame, pre, logic, audio, present, sched, post, idle, cpu_sum);

  g_acc = Acc{};
}
