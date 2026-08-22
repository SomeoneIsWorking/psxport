---
id: 12
title: Painter domains are ignored in real frames
status: resolved
symptom: Focused painter plans pass but shipping FPS60-captured frames still replay each producer object separately, leaving cross-object OT occlusion wrong
tags: gpu,renderer,painter,fps60,ordering
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

`RenderQueue::flush` always captured into `Fps60`, including the disabled interpolation tier, and `Fps60::presentPass` emitted individual items directly. The shipping path therefore bypassed `RenderQueue::emitQueue` and its painter planner. A logic frame can also contain several completed flushes whose layer/sequence ordering restarts; concatenating and globally planning those epochs is invalid.

## Resolution

Both direct queues and presentation now call `RenderQueue::emitItemStream`. `Fps60` builds a pointer-only merged stream, stamps every captured physical flush, and the shared emitter plans each flush independently. Focused tests cover pointers from separate backing queues and refuse mixed epochs. Full Clang CTest passed 83/83, and the real Spyro stage-13 diagnostic reported one 782-command authored range across three producer objects.
