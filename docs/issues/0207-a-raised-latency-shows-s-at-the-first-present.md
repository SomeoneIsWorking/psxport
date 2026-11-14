# 0207: a raised latency shows S at the first present

Status: open. Owner: `runtime/psx/frame/frame_presenter.cpp`, `FramePresenter::commitSealed` (`replayAt`), with
`runtime/psx/render/record_rasterizer.cpp`, `RecordRasterizer::show`.

## Observed
CTR, the Crash Cove pad replay, `ires=0`, `fps60=1`, sink 3840x2160, unpaced, `PSXPORT_DEBUG=presentseq`: step 2596
(the post-race load) takes 3 fields, the next intervals raise the latency one field, and fences 2599-2720 show
`t=0 composed=0` then `t=0.5 composed=1`. The order presented is S, then the in-between of S'→S, until a held
interval returns the latency to its least.

## Cause
At latency `lead + fields` the first present's `k` is 0. `replayAt` returns no record for `k < 1`, so the present
falls back to N's own present (S) instead of S', which the image already holds.

## Fix (not landed)
`replayAt` returns `from` at `k == 0`, and `RecordRasterizer::show` shows a composed record whose sequence is
`applied_` through `update` once `drawInBetween` refuses it. Both were tried on CTR, but this run has a raised latency
only on the load screen, whose chain is broken (every composed record there has `drawn=0` in `recordcheck`), so the
change could not be shown on a screen that interpolates. It lands with a reproduction on a chained screen.
