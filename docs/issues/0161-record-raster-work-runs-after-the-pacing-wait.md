# 0161: record raster work runs after the pacing wait, so a heavy field's present lands late

Status: fixed

## Observed
Spider-Man 1 real-window play check (2026-10-16, `psx-spider-man/scratch/play-check/play.py spiderman1 ... l1a2_t 75`,
shipping `config/aspect_16x9.ini`: `aspect=1 ires=0 light=1 shadows=1`, 2736x960 present image). After `warp l1a2_t`,
1043 of about 4500 presents miss (23%), `longest_miss_run=1`, p99 27 ms, with no player input. The misses fall on one
field in three, the field that carries the new game frame. The pacer always sleeps (6-14 ms): the guest step is not
over budget. The same route at `ires=1` misses 3 of 7258; `ires=0` with light and shadows off still misses 4.4%.

## Cause
`runtime/psx/frame/frame_presenter.cpp:FramePresenter::presentCurrent` waits for the deadline (`backend.pace`)
before `presentReal`, and `presentReal` reaches `GpuVkState::present` → `present_record` →
`RecordRasterizer::update` → `planRecord`. That is the whole record raster, and it runs after the wait.
`record_raster_setup.cpp:PlanBuilder::addTriangle` emits one `RecordQuad` per scaled scanline per plane and marks
each one in a `WrittenMap`, so its CPU cost grows with the scale. Timing the gap from the pacer's wake to the
`[present] sink` line over 2407 post-warp presents gives p50 0.5 ms, p90 9.3 ms and max 14.3 ms. 339 gaps exceed
half a period, against 342 misses in that run. `perf` puts `PlanBuilder::commit`, `addTriangle`'s span and
`PlanBuilder::emit` at about 25% of process CPU.

## Cause, in one sentence
`runtime/psx/frame/frame_presenter.cpp:FramePresenter::presentCurrent` (and `presentRecords` for the
in-between) called `backend.pace` before `presentReal`, so the whole record raster, the present-image pass
and the swapchain show ran after the deadline instead of only the show.

## Fix
A present is build, pace, show on every path: `FramePresentationBackend::buildReal`/`buildInBetween`,
`pace`, `show`; `DisplayPresenter::build`/`buildInBetween`/`buildHostPicture` and `show`;
`GpuVkState::build_display`/`build_image` submit the picture and `GpuVkState::show` only acquires, blits
and presents. Movies build before their field wait and show after it (`Fmv::buildFrame`/`showFrame`),
Tomba's SCEA card builds, paces and shows, and a picker pane tells its host at the build and at the show
(`psx::present::PaneHost`), so the selector composites before the panel's wait and shows after it.
`GpuVkState::present`, `present_image`, `present_screen` and the RmlUi choice screen they drew are gone.

## Measured after (2026-10-20, machine under load: load average 16-35 on 16 cores)
Spider-Man 1, the reproduction above, real vsynced window, shipping `aspect_16x9.ini` (ires=0 at
2736x960): `missed=351 (10.06%) longest_miss_run=5` over 3489 intervals of the whole run, boot included;
the 23% before was counted after the warp only. The product received a quit 34 s into the 45 s hold (`[session] Spider-Man was asked to quit`), not
from the script. Every later run was headless (no vsync): see the
Spyro numbers in the task report. What is left is the build itself overrunning the field: the record
plan's CPU cost grows with the internal scale (`record_raster_setup.cpp:PlanBuilder::addTriangle`), which
the split moves off the deadline but does not reduce.
