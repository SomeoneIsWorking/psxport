---
id: 26
title: Consumed RenderQueue is recaptured by an empty later DrawOTag
status: fix-verified
symptom: a physical draw boundary that submits zero new items duplicates the previous nonempty queue in the current frame capture
tags: render-queue,presentation,drawotag,tomba2
created: 2026-08-25
updated: 2026-08-25
---

## Root cause

`RenderQueue::flush` left `n` and the item payload intact after marking the queue consumed so that
`push()` could reset lazily. A later `DrawOTag` that submitted nothing called `flush()` again. Because
flush ignored the lifecycle bit, it sorted, diagnosed, ledger-counted, and captured the retained items
as a second physical epoch.

Tomba! 2 exposed the exact shape at cold-warp f3016: one real four-item submission contained one world
item attributed to node `0x800EDE28` plus three HUD items; a later empty draw boundary captured those
same four again. The resulting frame reported eight captured, six presented, and two apparent world
items. Node provenance made this initially look like two producer calls, but it was one queue replayed.

## Resolution

`RenderQueue::flush` now returns immediately when `consumed` is set, before every flush side effect.
The next real `push()` retains ownership of lazy reset. `test_render_queue_consumed_flush` proves the
old implementation RED and the corrected lifecycle GREEN, including the next-push behavior.

The one granted combined Tomba! 2 run changed f3016 to `captured n=0`, resumed Area 21 world rendering
at f3336, and ended with 3,614 reconciled frames and zero dropped layers. The candidate is verified in
this isolated worktree but is not landed on current psxport main.

## Ruled out

Moving the post-warp capture reset and gating Tomba's billboard-record producer on field-area-init were
both real-run falsified. Neither changed f3016. They were reverted; the producer was not the duplicate
owner.
