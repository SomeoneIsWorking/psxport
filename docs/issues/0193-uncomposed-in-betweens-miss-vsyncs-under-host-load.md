# 0193 — Uncomposed in-betweens miss vsyncs under host load

Status: open; the redundant raster is removed, the misses are not (2026-10-28). Owner: `frame/frame_presenter.cpp:FramePresenter::presentRecords` (the in-between
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
again on the image (`RecordRasterizer::advance`). Both pictures are S as the guest drew it.

## Fix
An uncomposed in-between builds N's own present (no composed record): the record is planned and rastered once per
interval and the `t = 1` present finds it applied. This is a cost reduction, not a demonstrated end to the misses.

Shown on the picture: in Crash 1 S is N-1 at almost every present (it shows the buffer N-1 drew while N draws the
other; `v2-c1-sn`, 2400 frames unpaced: 2394 held in-betweens and 2399 held `t = 1` presents with S != N, pairing each
`recordcheck` current record with its `presentseq` show). Fences 1860-1866 dumped unpaced (`v2-c1-dump`, S = N-1 at
every one) give in-between == `t = 1` present pixel for pixel at each fence while consecutive fences differ; the
`t = 1` present, unchanged by this fix, is the displayed buffer's rect of N's image, which S alone drew. Kept: the
dumps at 1/8 size in `v2-c1-dump/reduced`. The pass-9 old-against-new dumps (`c1-dump-old`, `c1-dump-fix`) were
deleted with the pass's full-size dumps, so that comparison is no longer checkable.

Shown on cost: under the controlled load the second build fell from 7.3 ms to 0.4 ms p50 (`c1-fix1-load6`) and
missed presents fell from 94/104 to 7 (`c1-final-load6`), the remaining ones whole presents at loads and
transitions. That did not hold up: the review's paced run at load 2.9-6.5 (`review-c1`) missed 12, longest run 3,
and six of its 14 long intervals were `t = 0.5` in-betweens (fences 154, 687, 839, 840, 936; 322 at `t = 0.75`).
Those runs had `PSXPORT_NOAUDIO=1`, so no audio thread or SPU output load. With audio on (SDL's dummy sink, so the
audio thread and SPU mix run without sound on the desktop), 2400 frames paced at load 1.2-2.9 (`v2-audio-c1`) missed
5, longest run 1; the long intervals are `t = 0.5` in-betweens at fences 2158-2163 (26-49 ms), each right after the
owner journal records a new `UnownedMoving` row, and fence 1548 (the 102 ms guest call). The load generator (`load.py`) is no
longer an accepted method (workspace rules: cost is measured in load-independent units on a quiet host).

## Owner journal lead (2026-10-28)
The `v2-audio-c1` misses right after new `UnownedMoving` rows are not journal cost. Unpaced from boot (`PSXPORT_NOPACE`,
`scratch/cost-audit/seq-base`, 2400 fences), every interval holding a new row is 3.6-6.8 ms (fences 3, 324, 434, 524,
629, 1551, 1552), the run's typical interval; a new row costs a find over the screen's rows and one `owners` line. The
long unpaced intervals are guest frames (fence 1548, 48 ms) and gameplay in-betweens of 10-13 ms at fences 1860-2185,
which hold a whole guest frame each.

## Paced runs by load (2026-10-28)
40 s paced from boot, all titles, with the 1-minute load sampled each second (`scratch/cost-audit/review-fix/paced*`,
`miss_by_load.txt` per title: intervals over 25 ms per 5 s window against the load then). Crash 1 missed 224 presents
(4.7%) in the first run of the day, whose load was sampled only at its start (5.5) and end (20.6); it did not repeat
(5 misses at load 10-22), so that run stays unexplained. Spyro missed 695 (17.5%) at load 18-24, the misses starting
15 s in while load stayed flat, and 9 (0.2%) at load 5.5-7.7 on the same binary. Crash Bash missed 271 (6.0%) at
load 21-37 and 108 (2.4%) at load 5-11, against 29 at load 6.4 earlier the same day; its misses do not follow load
window by window. None of these is a quiet-host result.

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
