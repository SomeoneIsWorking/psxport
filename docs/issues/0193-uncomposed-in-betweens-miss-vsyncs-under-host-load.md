# 0193 — Uncomposed in-betweens miss vsyncs under host load

Status: partly fixed (2026-10-28). Owner: `frame/frame_presenter.cpp:FramePresenter::presentRecords` (the in-between
present), `render/record_rasterizer.*` (the record raster it drives).

## Observed
Paced 2400-frame hidden runs from boot (fps60 1, ires 0, sink 3840x2160, `PSXPORT_DEBUG=presentseq`,
`scratch/player-bugs-8/`): Crash 1 missed 116 presents at host load 5.3 against 1 on a quiet host, every miss an
in-between `t=0.5 composed=0` 40-49 ms after the present before it (fences ~1840-2133). Reproduced in pass 9
(`scratch/player-bugs-9/`) with a controlled load (`load.py`, six workers pinned to CPUs 0-5, each streaming a 48 MB
buffer): `c1-base-load6` 94 missed, `c1-phase-load6` 104; the same binary quiet (`c1-base-quiet`) 1.

## Cause
The present path runs on the guest thread. Per 2-field frame it is guest work, the in-between build and the `t = 1`
build (temporary per-phase timing, `phases.py`): quiet, fences 1860-1960, guest 9.8 ms p50, each build 2.9 ms; under
the controlled load guest 21.6 ms, each build 6.8-7.3 ms, about 36 ms per 33.4 ms frame, so shows ran later each frame
until the pacer dropped whole intervals. Half of the build time was redundant: `FramePresenter::presentRecords` handed
an uncomposed in-between `&base` as its composed record, so `RecordRasterizer::drawInBetween` copied every plane and
planned and rastered S onto the in-between plane, and the `t = 1` present then planned and rastered the same record
again on the image (`RecordRasterizer::advance`). Both pictures are S as the guest drew it (pixel-equal,
`c1-dump-old` against `c1-dump-fix`, fences 1860-1865, interp and real).

## Fix
An uncomposed in-between builds N's own present (no composed record): the record is planned and rastered once per
interval and the `t = 1` present finds it applied. Shown: the 12 presents of fences 1860-1865 dumped unpaced are
pixel-identical before and after; under the same controlled load the second build fell from 7.3 ms to 0.4 ms p50
(`c1-fix1-load6`) and missed presents fell from 94/104 to 7, every remaining one a whole present at a load or
transition (fence 1548, a 102 ms guest call; fences 321, 627).

## Not fixed
- With more background load than the controlled six workers (other agents' builds and updates, load 5-21 on 16
  threads; old and new binaries interleaved, `ab2-*`, `ab3-*`) the new path still misses 50-175 presents against
  184-200 for the old one, most of them in-betweens 2-3 vsyncs late: the frame's remaining guest-thread work no longer
  fits the field when the guest thread shares its cores. The guest (Lightrec,
  Beetle's software GPU for the device VRAM, the owner ledger per store) is the largest share, 21 ms of a loaded
  frame.
- The guest thread sleeps until an in-between's deadline before building the `t = 1` present (`DisplayPresenter`
  holds one scheduled show and `FramePacer::deadline` finishes it before planning the next), so the next frame's guest
  work starts only after the in-between is shown, 10-14 ms per frame spent asleep. The proper fix queues the
  interval's shows: a held interval shows one build at each of its deadlines, and a composed one builds into a second
  present image, with the pacer planning from the queued deadlines instead of finishing the show first. It changes
  pacing for every title, so it is its own change with the all-title real-time check.
