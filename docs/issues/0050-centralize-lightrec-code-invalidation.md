---
id: 50
title: Centralize executable-image and override invalidation through Lightrec
status: open
symptom: executable-memory writes and module-generation changes have no single path that invalidates all affected translated and native-dispatch decisions
tags: lightrec,invalidation,dma,overlays,savestate
created: 2026-09-04
updated: 2026-09-04
---
state_items: S015

## Root cause

Offline-generated guest bodies did not need a runtime code-cache invalidation contract. Lightrec does.
PSX executable RAM can change through ordinary CPU writes, DMA, disc/module loads, decompression,
debugger writes, and savestate restoration. Overlay slots also reuse addresses, so retaining either a
translated block or an override decision after the image generation changes executes stale code.

## Required outcome

Add one per-`Core` invalidation owner that normalizes KSEG aliases, receives exact post-write ranges
from every executable-memory writer, updates image generations, and calls Lightrec's supported
invalidation API. Override install/remove/replace events use the same owner when translated call paths
can capture dispatch policy. Lightrec continues to own cache storage and executable memory; psxport
only determines which PSX ranges and decisions became stale.

Tests must cover an overlapping changed write, adjacent/out-of-range write, DMA/module replacement,
same-address new generation, savestate restore, and override-policy change. Reports include ranges
examined, blocks/decisions invalidated, and the zero-overlap answer.
