---
id: I009
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

test_cdc_bfrd_split_dma shipping CDC request/FIFO test

## Validated by

On the pre-fix cdc_write implementation, the exact split-DMA case failed after 4 checks with loc_lba=17 instead of 16; its independent deassert-then-assert discriminator passed 5 checks. A second RED proved deasserted BFRD incorrectly left DRQSTS set. With latch semantics the suite passes 3/3 and 535 checks, proving preserve-current-sector, request-next-sector, and deasserted-access outcomes.

## Known failure modes

(none recorded yet)
