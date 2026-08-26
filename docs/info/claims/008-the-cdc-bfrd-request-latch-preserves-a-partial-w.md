---
id: C008
kind: claim
status: holds
created: 2026-08-21
tags: cdc,cdrom,dma
depends: runtime/recomp/cdc_native.cpp#write_request_register, tests/test_cdc_bfrd_split_dma.cpp#test_repeated_assertion_preserves_split_dma_cursor
reconfirmed: 2026-08-26
verified_at: 2026-08-26 22:24:51
---

## Claim

The CDC BFRD request latch preserves a partial whole-sector FIFO across repeated asserted writes.
A deassert/assert transition controls FIFO access but can advance only to a following sector already
made ready by the independent drive deadline.

## Evidence

`test_cdc_bfrd_split_dma` drives begin/read/write/DMA with synthetic LBA16/17 sectors: 3/3 tests,
535 checks. Repeated assertion and a deassert/assert transition before the next drive deadline both
retain LBA16 and its cursor; deassertion clears DRQSTS and blocks CPU/DMA access. The continuous-read
suite independently advances the clock before expecting the transition to install LBA17. Real Crash
Bash still reads the PVD as 3+512 words from LBA16 and reaches `load file start`.

## What would falsify it

Any repeated asserted BFRD write changes loc_lba/data_rd; a pre-deadline deassert/assert advances;
a post-deadline transition fails to install the ready sector; deasserted access succeeds; or a real
consumer receives bytes from the wrong LBA/cursor.

## Re-confirmed 2026-08-21

Reconfirmed against the deterministic drive-deadline model: BFRD suite 3/3 with 535 checks and
continuous ReadN suite 5/5 with 64 checks.

## Re-confirmed 2026-08-21

Post-landing Clang CTest passed 77/77; the shipping BFRD suite passed its repeated-assertion, split-DMA, deasserted-access, and following-sector answers.

## Re-confirmed 2026-08-26

Fresh Clang full suite at the typed PlatformHle milestone passed test_cdc_bfrd_split_dma and test_cdc_continuous_read in 105/105 CTests after the XA wall-clock CDC change.
