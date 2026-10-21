# 0168: one-field logic frames got an in-between; the in-between half overruns its deadline

Status: partly fixed. Owner: `runtime/psx/frame/frame_presenter.cpp` `FramePresenter::presentRecords`,
`runtime/psx/present/emit_memory.cpp` `HostMemory`.

## Observed
Spyro 1 (psx-spyro issue 0196), fps60, 16:9, real speed, unmapped window,
`PSXPORT_DEBUG=presentseq` (one line per present: host time, record shown, from, t, fields):
- A logic frame held for one field (Spyro 1's "THE ADVENTURE BEGINS" card) still got an in-between,
  so two presents shared one field: 8.3 ms intervals, 120 presents a second.
- Gameplay (`warp 10`, turning, 20 s): 572 logic frames (28.2 Hz), intervals 9..35 ms, playcheck
  missed 3.91%. perf: main thread about 0.91 busy; `composeFrame` 43% inclusive, `HostMemory`
  `std::map` lookups 23% self.

## Cause
- `FramePresenter::presentRecords` gave every logic frame two presents whatever its field count.
- `HostMemory` (`emit_memory.cpp`) looked up every guest word a producer's re-run read through a
  `std::map` of ranges and a `std::map` overlay.
- The two presents of a 30 Hz frame are not equal work: the in-between's half carries guest logic,
  the guest draw, the in-between's composition and both builds' CPU work; the real half carries only
  its pace and show. Under load the first half runs past its 16.7 ms deadline.

## Fix
- `presentRecords`: a logic frame of fewer than two fields gets one present at `t = 1`. The card now
  presents at 16.7 ms.
- `HostMemory`: the overlay is a sorted vector with one entry per word, and lookups try the range the
  last lookup found first. Gameplay after: 607 logic frames in 20.27 s (29.95 Hz), missed 1.08%.

## Left
- Gameplay intervals still spread 8..30 ms (382 of 1213 in the 17 ms bucket): 462 of 620
  in-between presents had no wait left, while real presents had a median 6.9 ms. Proposed fix: a
  producer exact at `t = 1` answers `Composition::Recorded` there (no terrain rebuild for the real
  present), and the real present built after the in-between is shown after the next logic frame's
  work starts, so both halves carry about the same work. That changes the presenter's order and
  needs a design decision.
- A logic frame of three or more fields still gets two presents at `t = 0.5`.
- Present phase against the display: the pacer targets 59.94 Hz and MAILBOX never waits, so on a
  60.00 Hz display each present drifts against vblank and a repeat or skip lands every few seconds.
  An unmapped surface is never scanned out, so a tool run cannot see it; the fix is pacing to the
  display's refresh (presentation feedback) rather than the guest field rate.

## Proof
Spyro 1 boot to title at real speed after the change: `[playcheck] presents: intervals=2445
p50=16.75ms p99=17.00ms worst=47.71ms missed=4 (0.16%)`, `records: repeats=0 backwards=0`.
