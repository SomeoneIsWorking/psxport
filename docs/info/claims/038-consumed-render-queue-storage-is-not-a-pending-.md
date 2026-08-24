---
id: C038
kind: claim
status: holds
created: 2026-08-25
tags: render-queue,presentation,lifecycle
depends: runtime/recomp/render_queue.cpp#RenderQueue::flush, runtime/recomp/render_queue.cpp#RenderQueue::push, tests/test_render_queue_consumed_flush.cpp
---

## Claim

A consumed RenderQueue payload is retained storage for the next push's lazy reset, not a pending
physical flush epoch. Until another producer pushes, `RenderQueue::flush` has no sort, diagnostic,
ledger, capture, or emit side effect.

## Evidence

`test_render_queue_consumed_flush` was RED before the lifecycle guard: a second empty flush raised the
shipping FramePresenter capture count from 1 to 2. It is GREEN with the guard: presenter and HUD-ledger
counts remain 1 across the empty flush, then a real push performs the existing lazy reset and advances
both counts to 2. The focused Clang 22 gate passes 6/6 queue, presenter, painter, and VRAM tests; direct
clang-tidy and `git diff --check` pass. A bounded Tomba! 2 consumer run
`Tomba2Engine/scratch/logs/gate-run-consumed-flush-20260825.log` changed the former f3016 8-captured /
6-presented drop to `captured n=0`, resumed destination world capture at f3336, and reconciled 3,614
frames with zero dropped layers.

## What would falsify it

A second flush with no intervening push changes FramePresenter capture state or PresentLedger counts,
emits retained items in diff mode, or the same Tomba! 2 cold-warp sequence again captures any item at
f3016 after both framework fixes are present.
