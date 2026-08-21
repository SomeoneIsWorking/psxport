---
id: 8
title: DMA3 FIFO depletion leaves stale guest RAM instead of the controller's zero value
status: resolved
symptom: Spider-Man's stock libstr asks for 504 DMA3 words with a 70-word raw-sector tail available; psxport writes only 70, then poison 0x0E0E0E0E reaches the guest allocator
tags: cdc,dma3,libstr,spider,zero-fill,stale-memory
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

The framework treated the CDC FIFO-sourced word count as the DMA channel's completed word count.
When the FIFO depleted, `mem.cpp` wrote only those words and preserved whatever bytes already occupied
the rest of guest RAM. That is not the controller contract.

Spider-Man supplies the exact discriminator. In whole-sector ReadS mode it consumes 12 PIO bytes,
then DMA3 reads 8 + 504 words, leaving the 280-byte raw-sector tail: exactly 70 words. Stock libstr
then deliberately starts another fixed 504-word DMA before the next sector. The pre-fix port reports
`guest asked 504 words, FIFO held 70`, writes only 70, and later reaches the allocator with poison
`0x0E0E0E0E`.

The vendored oracle has two independent rules that resolve the missing 434 words. Beetle
`dma.c::ChCan(CH_CDC)` keeps the channel runnable, including when the FIFO is empty, and
`cdc.c::PS_CDC_DMARead` initializes each returned word to zero before copying any available FIFO
bytes. An empty or depleted controller read therefore returns zero; it does not preserve RAM and it
does not fetch a future sector.

## Resolution

`cdc_dma_read` now initializes every programmed output word to the controller's zero value, overlays
the words actually available from the current FIFO, and returns the FIFO-sourced count. The DMA3
owner commits the full programmed count and logs one exact denominator: FIFO words plus
controller-zero words. The diagnostic remains able to expose unexpected depletion without calling
normal controller zeros disc data.

`test_cdc_dma_depletion` covers the shipping CDC and Core DMA3 paths with Spider-Man's exact
504/70 split. It rejects stale destination bytes, rejects bytes fabricated from a ready following
sector, and proves a transfer with BFRD deasserted completes entirely with controller zeros. The
pre-fix short-write mutation fails 0/3; the candidate passes 3/3 with 1,518 checks.

## Consumer verification

- Spider-Man's exact repeat transfer reports `FIFO 70 + controller-zero 434`; no poison allocator
  read occurs. The regenerated Native gate reaches dem1 at frame 329 and l1a1 at frame 478. The
  zero-argument project route independently reaches dem1 at frame 315 and l1a1 at frame 456. Both
  stop later at the already-separate FPS60 render-queue capacity boundary, not CDC or allocator code.
- Crash Bash preserves its split 3+512-word PVD read and one 189-sector load, with zero
  controller-zero words observed, then reaches its existing 0x80092BDC recompilation boundary.
- Toy Story 2 remains bounded at 358 sectors over 11 phases, with zero controller-zero words and no
  LBA burst, then reaches its existing BITS/MEMORY boundary.
- Vagrant Story completes five ReadN ranges totalling 384 sectors and preserves Pause arbitration;
  all 768 observed DMA3 transfers are FIFO-backed, then TITLE.PRG reaches the existing 0x80071334
  overlay boundary.

The final Clang framework suite passes 78/78; clang-format covers 306 first-party files and
clang-tidy covers all three touched translation units.
