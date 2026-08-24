---
id: 27
title: Standalone REPL warp replaces guest state before presenting the pending frame
status: fix-verified
symptom: a cold dev warp makes presentation consume old-scene capture metadata against destination-scene guest state
tags: repl,warp,presentation,frame-boundary,tomba2
created: 2026-08-25
updated: 2026-08-25
---

## Root cause

The standalone `game_main` loop serviced an armed REPL warp before calling `native_step_frame`.
`native_step_frame` owns the presentation of the queue captured during the prior guest tick, so the
warp replaced the scene first. A temporal presenter then tried to rebuild the pending old picture
from destination-scene state and dropped the old world layer.

The fault was ownership order, not Tomba-specific loading policy: the framework decides when the
standalone command is serviced, while the title's `GameHooks::devWarp` owns the cold operation itself.

## Resolution

`standalone_frame_boundary` is the one explicit owner of the order: present pending, begin capture,
apply optional standalone warp, run guest frame. `native_step_frame` uses it; direct dual-core stepping
passes `serviceStandaloneWarp=false`, preserving SBS's separate transaction.

The red-first hermetic trace rejects the old order and accepts armed plus unarmed corrected paths. In
the bounded Tomba! 2 consumer run, f3015 presents all 168 old-scene items before the cold Area 21 warp,
and f3016 contains no stale old-scene capture. The candidate is verified in this isolated worktree but
is not landed on current psxport main.

## Ruled out

Moving capture reset after the warp was tested and then real-run falsified: it did not change the next
f3016 mismatch. The reset stays before warp and before destination producers; the later residual was a
separate consumed-RenderQueue lifecycle bug.
