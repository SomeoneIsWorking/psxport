# 0158: held 30 Hz pictures count as missed vsyncs

Status: open (needs a decision)

## Observed
Tomba! 2 real-window play check (2026-10-16, `warp 1`): `presents: intervals=7336 over_1.5_vsync=1743 (23.76%)
longest_miss_run=305`. Gameplay after the warp adds none (intervals 3000 to 7336, misses 1743 to 1743). All of
them are before it: the SCEA card stage (10 s, pacer `quota=2` on every call, 30 presents/s, the 305-long run),
`MOVIE/LOGO.STR` and the 67 s `MOVIE/OP.STR` (p50 33.5 ms, p99 54.75 ms). The Start button does not skip
OP.STR.

## Cause
A picture the title holds for two fields is presented once: `runtime/psx/frame/frame_presenter.cpp:
FramePresenter::presentCurrent` paces the whole `guestFields` count and then presents once for a frame with no
in-between (Tomba! 2's boot cards run at 2 fields per frame), and `runtime/psx/movie/native_fmv.cpp:
Fmv::presentFrame` presents once per movie frame. Every such interval is over 1.5 vsyncs, which is what
`runtime/psx/debug/gpu_perf.cpp:GpuPerf::presented` counts as a miss. On screen the swapchain holds the image,
so the 33 ms intervals look as on a console; the 50 ms ones inside OP.STR are real late frames.

## Gap
The §6 bar counts a held picture as a miss, and a re-present of the same record would count as a repeat.
Either the presenter re-presents a held picture every field (FMV and non-interpolated 2-field frames, every
title), or the play-check counter judges an interval against the cadence the pacer was given. That choice
changes the bar or every title's present path, so it is not made here.
