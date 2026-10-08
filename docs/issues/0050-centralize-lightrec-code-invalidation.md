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

## Finding 2026-09-29: an interior-word write never revokes its block

`lightrec_invalidate(addr, len)` (`shared/lightrec/lightrec.c`) only zeroes the code-LUT entries of the
written words, and a block is re-validated (`lightrec_block_is_outdated`, `blockcache.c`) only when the
LUT entry at the block's *start* PC is zero. So a write to any word after the first word of a translated
block is reported to `notifyExecutableWrite`, counted in `ExecutorCounters::invalidations`, and has no
effect: the stale block keeps executing. This applies to every writer through the owner (CPU
`MappedStore`, DMA, module loads, savestate-style restores), not to one caller.

Reproduced with `tests/test_override_differential.cpp`: a two-instruction function `jr $ra; addiu $v0,
$zero, 1` was translated, its delay-slot word was rewritten to `addiu $v0, $zero, 2` and reported to
the owner, the invalidation counter advanced, and the next call still returned 1. The same fixture
patching the block's FIRST word returns 2, and that form is what the test now asserts. The fix belongs in
the Lightrec fork, which has to revoke every block overlapping the range (the block cache knows each
block's span). Widening ranges on the psxport side cannot fix it because psxport does not know where
blocks start.

## 2026-09-29: the Lightrec half landed

`shared/lightrec` `20bc8a2` makes `lightrec_invalidate(addr, len)` revoke every translated block whose
extent overlaps the range (previously only a block whose FIRST word was written); its
`invalidation_contract` test fails 4 of 11 cases on the prior tree and passes 11/11. psxport pins it.
Still open here: `LightrecExecutor` must publish Lightrec's new denominators (`invalidation_words`,
`invalidation_guards`, `invalidation_scans`, `invalidated_blocks`) beside `ExecutorCounters::invalidations`,
and the per-Core owner/test list above.
