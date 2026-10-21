# 0168: one-field logic frames got an in-between; the in-between half overran its deadline

Status: fixed. Owner: `runtime/psx/frame/frame_presenter.cpp` `FramePresenter::presentRecords`,
`runtime/psx/present/display_presenter.cpp` `DisplayPresenter::showAt`, `runtime/psx/frame/frame_pacer.cpp`.

## Observed
Spyro 1 (psx-spyro issue 0196), fps60, 16:9, real speed, unmapped window, `PSXPORT_DEBUG=presentseq`:
- A logic frame held for one field ("THE ADVENTURE BEGINS" card) still got an in-between, so two presents
  shared one field: 8.3 ms intervals.
- Review of 830d3e53, new-game cutscene, two real-speed runs on a loaded host: a burst of about 17 late
  presents in 0.8 s, 24-26 s into the cutscene (records 2365-2387 and 2460-2482 at different offsets); each
  in-between 20-53 ms late and the real present 6-10 ms after it. Worst 61.5 / 57.1 ms, 15 intervals over
  20 ms per run.

## Reproduction
`psx-spyro/scratch/judder-fix/realtime.py <tag> intro` under host load (`loaded.py 14 -- ...`: 14 busy
processes on 16 threads), build 830d3e53: records 1700-3100, 733 intervals over 20 ms, worst 292 ms. At no
load the same build had no late present. The `pacer` channel's sleep before each present, no load:
in-between median 7.1 ms (p1 3.2 ms, 8 with none), real present median 12.1 ms. The in-between half
carried about 9.6 ms of work and the real half 4.6 ms.

## Cause
- `FramePresenter::presentRecords` gave every logic frame two presents whatever its field count.
- `FramePresenter::presentRecords`/`presentCurrent` paced and showed inline: after the t = 1 show the
  guest ran the next logic frame, then the in-between was composed and built, all before the in-between's
  deadline 16.7 ms later; the t = 1 present's half held only its own composition and build. Guest logic
  could only ever use the first half, so a slowed host (load, a heavier frame) made the in-between late while
  the other half sat idle, and the deadline grid then pulled the t = 1 present early.

## Fix
- A logic frame of fewer than two fields gets one present at `t = 1`.
- A present is built, then its show is scheduled (`DisplayPresenter::showAt`) for the deadline
  `GameRuntime::presentDeadline` plans, and control returns to the guest. The executor shows it at the first
  segment boundary past the deadline (`DisplayPresenter::showIfDue`), or anything that would build, show,
  repaint or pace first waits for it (`finishShow`); `FramePresenter::commit` finishes it before composing
  the next frame. Logic, both compositions and both builds now share the whole 33.4 ms; only the in-between's
  composition and build stay in front of its deadline.
- `GameRuntime::pacePresentation` became `presentDeadline` (plan without sleeping); `FramePacer` plans
  (`subframeDeadline`, `presentedFieldsDeadline`) and its waiting calls plan, then sleep (`sleepUntilMs`).
- `HostMemory`: the overlay is a sorted vector and lookups try the last range found first; copy and move
  are deleted, since that cache points into the object's own ranges.

## Proof (Spyro 1, fps60, 16:9, real speed, unmapped window, `PSXPORT_SHOW_WINDOW=0` and no
`PSXPORT_NOPACE` read from `/proc/<pid>/environ`; lateness = show time minus `due`)
- New-game cutscene, records 1700-3100, two runs at no extra load: 2802 presents, p50 16.68 ms, p99
  16.80 / 16.79 ms, worst 16.98 / 17.12 ms, none over 20 ms; every present within 0.7 ms of its deadline;
  1401 of 1401 in-betweens composed.
- Same, 8 busy processes: p99 16.84 ms, one stall (a t = 1 present 57 ms late, record 1712, nothing logged
  between the logic's end and it); otherwise every present within 1.6 ms.
- Same, 14 busy processes: 616 intervals over 20 ms, worst 241 ms (before: 733, 292 ms). The host is
  oversubscribed there; the frame's total work no longer fits 33 ms, which no ordering can fix.
- Gameplay `warp 10` turning, 20 s: 1218 presents, p50 16.68 ms, p99 16.86 ms, worst 18.28 ms, none over
  20 ms; in-betweens within 0.6 ms of their deadline, t = 1 presents within 1.8 ms. Run end:
  `[playcheck] presents: intervals=3518 p50=16.75ms p99=17.00ms worst=52.86ms missed=5 (0.14%)`, records
  repeats 0, backwards 0.
- Boot to the title screen, 40 s: 2453 presents, p50 16.68 ms, p99 16.78 ms.

## Left
- A logic frame of three or more fields still gets two presents at `t = 0.5` and `t = 1`.
- Present phase against the display: the pacer targets 59.94 Hz and MAILBOX never waits, so on a
  60.00 Hz display each present drifts against vblank and a repeat or skip lands every few seconds.
  An unmapped surface is never scanned out, so a tool run cannot see it; the fix is pacing to the
  display's refresh (presentation feedback) rather than the guest field rate.
