---
id: 19
title: Already-60fps ports still depend on the interpolation owner for frame commit
status: resolved
symptom: Mega Man X4's game/core/vsync_sync.cpp calls c->game->fps60.frame_commit(c,1) even though the title is already 60fps and should own no interpolation capture/queue state.
tags: fps60,interpolation,presentation,architecture,megamanx4
created: 2026-08-22
updated: 2026-08-22
---

Root cause: current-frame capture, real presentation, field pacing, diagnostic capture, and ledger
reconciliation were methods/state of `Fps60`. Calling the ordinary frame boundary therefore required
constructing the interpolation owner even when its active toggle was false.

Resolved by `FramePresenter`, the neutral per-Game frame fence described in
`docs/presentation-contract.md`. `Fps60` is now an optional `TemporalFramePresentation` decorator
created through `GameRuntime`; direct runtimes create none. `test_frame_presenter_contract` drives the
shipping state machine with no temporal product, while `test_direct_runtime_no_temporal_link` proves
the direct executable contains no concrete `Fps60` symbols. Consumer repos still need to migrate their
frame-boundary call sites and pins.
