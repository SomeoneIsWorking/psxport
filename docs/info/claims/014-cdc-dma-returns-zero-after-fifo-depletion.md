---
id: C014
kind: claim
status: holds
created: 2026-08-21
tags: cdc,dma3,fifo,zero-fill
depends: runtime/recomp/cdc_native.cpp#cdc_dma_read, runtime/recomp/mem.cpp#Core::io_write, tests/test_cdc_dma_depletion.cpp#test_dma3_shipping_path_commits_the_full_programmed_transfer
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:50:54
---

## Claim

CDC DMA3 commits every programmed guest word. Words available from the current data FIFO retain
their source bytes; every word after FIFO depletion is the controller's zero read value. Depletion
never preserves stale destination RAM and never borrows bytes from a following sector.

## Evidence

The authoritative vendored path keeps CDC DMA runnable in `dma.c::ChCan(CH_CDC)` and
`cdc.c::PS_CDC_DMARead` constructs a zero word before copying any available FIFO bytes.

`test_cdc_dma_depletion` drives the shipping `cdc_dma_read` and Core DMA3 CHCR paths with the measured
Spider-Man request: 504 programmed words and 70 FIFO tail words. It proves 70 exact tail words plus
434 zeros, preservation of a ready following sector, and an all-zero completed transfer when BFRD is
deasserted: 3/3 tests and 1,518 checks. Restoring the old short-write behavior makes all three tests
fail on the first stale `0x0E0E0E0E` word (0/3, 3 failures), then the candidate returns to 3/3.

The real Spider-Man consumer reports the same `FIFO 70 + controller-zero 434` denominator and
advances through dem1/l1a1 on both the regenerated Native gate and zero-argument project route,
without the prior poison allocator fault. Focused Crash Bash, Toy Story 2 and Vagrant Story
regressions reach their established later boundaries; every observed DMA3 transfer in those three
consumers remains fully FIFO-backed, proving the new behavior is inactive where no depletion occurs.

## What would falsify it

Any programmed DMA3 destination word retains its previous RAM value after FIFO depletion; a zero
suffix contains bytes from a following sector; BFRD-deasserted DMA exposes internal FIFO bytes;
the diagnostic's FIFO plus controller-zero denominator differs from the programmed word count; or a
real consumer repeats Spider-Man's post-underrun poison/allocator abort on this implementation.

## Re-confirmed 2026-08-22

Combined Clang framework build and full 91/91 CTest pass at f468f2c7, including CDC deterministic-time controls, HSync counter coverage, and DMA regression coverage.
