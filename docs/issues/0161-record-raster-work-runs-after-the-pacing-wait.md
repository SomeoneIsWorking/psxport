# 0161: record raster work runs after the pacing wait, so a heavy field's present lands late

Status: open

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

## Fix (not made)
Rasterize the record before the wait, and keep only the swapchain show after it. `GpuVkState::present` must
split into building the picture (record raster plus present image) and showing it (`show_present_image`), and
`FramePresenter` must pace between the two for the real present and for the in-between. This changes when every
title presents, so it needs a real-window check on every title. The per-scanline plan cost at scale is a separate
cost; the split removes it from the deadline but does not reduce it.
