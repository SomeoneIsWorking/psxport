# 0158: held 30 Hz pictures count as missed vsyncs

Status: fixed

## Observed
Tomba! 2 real-window play check (2026-10-16, `warp 1`): `presents: intervals=7336 over_1.5_vsync=1743 (23.76%)
longest_miss_run=305`. Gameplay after the warp adds none (intervals 3000 to 7336, misses 1743 to 1743). All of
them are before it: the SCEA card stage (10 s, pacer `quota=2` on every call, 30 presents/s, the 305-long run),
`MOVIE/LOGO.STR` and the 67 s `MOVIE/OP.STR` (p50 33.5 ms, p99 54.75 ms). The Start button does not skip
OP.STR.

## Cause
`runtime/psx/debug/gpu_perf.cpp:GpuPerf::presented` judged every present-to-present interval against one
field (1.5 refresh periods), while `runtime/psx/frame/frame_presenter.cpp:FramePresenter::presentCurrent`
and `runtime/psx/movie/native_fmv.cpp:Fmv::presentFrame` present once per picture held for two or more
fields (Tomba! 2's boot cards at 2 fields per frame, 30 fps movies), so every held picture counted as a miss.

## Fix
A miss is a present that lands late against the deadline its pacer gave it. `FramePacer` reports the
fields each plan holds the next present for (`GpuPerf::heldFor`, quota / parts) and `Fmv::presentFrame`
reports the movie's frame period in fields from its media clock (`Fmv::heldFields`). `GpuPerf::presented`
counts an interval as missed when it is over held fields + 0.5 refresh periods; a present nothing paced is
held for one field. Test: `tests/test_gpu_perf_reports.cpp`
(`a_present_misses_only_half_a_period_past_its_held_fields`). The 50 ms intervals inside OP.STR still count.

## Boot walk at 60 fps (2026-10-22)
Tomba! 2 from boot at real speed, 8x on a 3840x2160 sink, fps60, hidden window, display simulated at 60.00 Hz from
`presentseq` (psx-tomba `scratch/judder-check/walk-b3`, load 1.6 to 1.0).
- The SCEA card fade stepped at 30 Hz: psx-tomba `BootCards::stepScea` built one picture per two fields. It now
  presents each field, the second at the fade halfway to the next card frame's: 574 presents, 1 repeat.
- Movies were invisible to `presentseq`: `Fmv::showFrame` showed through `DisplayPresenter::show`, which logs
  nothing. It schedules through `showAt` on the host clock. `OP.STR`: 2027 presents held 2 fields each, 8 repeats
  and 5 drops in 67 s (the 59.94 Hz media clock slipping against a 60.00 Hz display). `LOGO.STR` lost 14 vblanks to
  one 223 ms disc hunk fill (`[disc] worst fill 222.9 ms`); a run with the hunks cached shows none.
