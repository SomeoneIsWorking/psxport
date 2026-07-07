// gpu_perf.h — per-frame CPU phase / frame-time profiler API (impl gpu_perf.cpp; REPL `debug perf`).
// Phases: 0 = LOGIC, 1 = AUDIO, 2 = PRESENT, 3 = SCHED. Hooks are one cached-int branch when off.
#pragma once

void perf_frame_begin(void);       // top of native_step_frame: start the frame clock
void perf_mark_pre(void);          // boundary between pre-tick host work and the guest tick
void perf_phase_begin(int phase);  // open a timed phase
void perf_phase_end(int phase);    // close it, charging elapsed time to the phase slot
void perf_frame_end(void);         // bottom of native_step_frame: close frame + rolling avg
