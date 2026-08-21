---
id: C008
kind: claim
status: holds
created: 2026-08-21
tags: cdc,cdrom,dma
depends: runtime/recomp/cdc_native.cpp#write_request_register, tests/test_cdc_bfrd_split_dma.cpp#test_repeated_assertion_preserves_split_dma_cursor
reconfirmed: 2026-08-21
verified_at: 2026-08-21 11:35:45
---

## Claim

The CDC BFRD request latch preserves a partial whole-sector FIFO across repeated asserted writes and advances only after deassertion followed by assertion.

## Evidence

test_cdc_bfrd_split_dma drives cdc_begin_read, cdc_write and cdc_dma_read with synthetic LBA16/17 sectors: 3/3 tests, 535 checks. Its pre-fix RED was loc_lba 17 versus 16 after the repeated assertion, while its deassert/reassert opposite-answer case passed; the third case proves deasserted BFRD clears DRQSTS and blocks FIFO access. Real Crash Bash reaches its 512-word PVD DMA at LBA16 with data_rd=12 and bytes 01 CD001 01 00. The 105-line consumer log contains 0 `Cant find CRASHBSH.DAT` messages and 1 `load file start` (Crash Bash docs/issues/0005; scratch/logs/cdc-latch-consumer.log and cdc-latch-gdb.log).

## What would falsify it

Any repeated asserted BFRD write changes loc_lba/data_rd or any deassert-then-assert transition fails to present the following sector; or a real consumer receives bytes from the wrong LBA/cursor under either sequence.

## Re-confirmed 2026-08-21

Post-landing Clang build and 75/75 CTests passed; the BFRD split-DMA shipping suite remains 3/3 with 535 checks while the continuous-ReadN suite passes 3/3.
