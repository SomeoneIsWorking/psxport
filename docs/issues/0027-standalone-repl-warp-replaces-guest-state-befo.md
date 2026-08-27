---
id: 27
title: Standalone REPL warp replaces guest state before presenting the pending frame
status: resolved
symptom: a cold dev warp makes presentation consume old-scene capture metadata against destination-scene guest state
tags: repl,warp,presentation,frame-boundary,tomba2
created: 2026-08-25
updated: 2026-08-27
---

## Root cause

The former shared loop serviced an armed REPL warp before delegating the title frame. Presentation of
the queue captured during the prior title tick therefore ran only after the warp had replaced the
scene. A temporal presenter then tried to rebuild the pending old picture from destination-scene
state and dropped the old world layer.

The fault was ownership order. It was temporarily implemented in shared boot code even though both
the cold operation and its presentation boundary are properties of Tomba! 2's frame transaction.

## Resolution

The current owner is `Tomba2Engine/game/core/frame_driver.cpp#TombaFrameDriver::stepFrame`. Its finite
order commits the pending presentation, resets Tomba's capture epoch, applies the armed standalone
warp, and only then steps destination guest work. SBS is excluded inside
`applyArmedStandaloneWarp`. `Tomba2Engine/tools/verify_native_frame_contract.py` checks this title-local
order and produces an opposite answer for reversed fixtures. The obsolete shared helper and its test
were deleted when the framework frame body moved to the title.

The bounded Tomba! 2 consumer evidence remains: f3015 presented all 168 old-scene items before the
cold Area 21 warp, and f3016 contained no stale old-scene capture.

## Ruled out

Moving capture reset after the warp was tested and then real-run falsified: it did not change the next
f3016 mismatch. The reset stays before warp and before destination producers; the later residual was a
separate consumed-RenderQueue lifecycle bug.
