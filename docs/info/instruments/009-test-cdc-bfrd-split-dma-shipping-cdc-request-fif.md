---
id: I009
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

test_cdc_bfrd_split_dma shipping CDC request/FIFO test

## Validated by

On the pre-fix cdc_write implementation, the exact split-DMA case failed after 4 checks with
loc_lba=17 instead of 16. A second RED proved deasserted BFRD incorrectly left DRQSTS set. The suite
now passes 3/3 and 535 checks: repeated assertion and deassert/assert before a drive deadline preserve
the current sector, while deassertion blocks access. Following-sector timing belongs to I012.

## Known failure modes

(none recorded yet)
