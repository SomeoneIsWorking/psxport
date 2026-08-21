---
id: C012
kind: claim
status: holds
created: 2026-08-21
tags: cdc,cdrom,readn,dma
depends: runtime/recomp/cdc_native.cpp#announce_following_sector, tests/test_cdc_continuous_read.cpp#test_partial_fifo_does_not_block_following_sector_event
reconfirmed: 2026-08-21
verified_at: 2026-08-21 11:36:00
---

## Claim

The CDC continuous ReadN model announces one following sector independently of partial BFRD FIFO drainage and installs it only when software makes a later data-service request.

## Evidence

test_cdc_continuous_read drives the shipping cdc_begin_read/cdc_write/cdc_dma_read path with hermetic raw sectors. Before the fix its partial-FIFO and full-drain cases both saw pending IRQ type 0 instead of 1; the original stopped opposite passed. The final strengthened suite passes 3/3 and 31 checks: a 2060/2340-byte partial FIFO retains LBA16/cursor while exactly one following INT1 is pending, Pause produces its INT3/INT2 responses and no data event, and a full drain only empties/rearms BFRD before the next request installs LBA17. test_cdc_bfrd_split_dma remains 3/3 and 535. Real Crash Bash built against the isolated framework worktree DMA-read LBA35799 as 3+512 words, left 280 bytes, observed pending E1 and acknowledged it, then BFRD 00->80 loaded LBA35800 and both DMA legs completed; it continued through LBA35987 (189 sectors per pass, 21 passes), with no Cant-find or recomp-miss. Tomba's bounded 120-frame route matched its clean baseline exactly but is explicitly negative coverage: it performed no asserted BFRD or DMA.

## What would falsify it

A partial accepted FIFO suppresses the following data-ready event, changes LBA/cursor before a later BFRD service request, emits more than one event for repeated asserted BFRD, emits a data event after Pause, or a real consumer fails to reach the next LBA unless it drains the raw-sector tail.

## Re-confirmed 2026-08-21

Reconfirmed after the isolated Clang build and 75/75 CTests, targeted final continuous-read test 3/3 with 31 checks, prior BFRD suite 3/3 with 535 checks, real Crash Bash LBA35799-to-35800 transition and 189-sector pass, and bounded Tomba no-regression comparison.

## Re-confirmed 2026-08-21

Post-landing Clang build and 75/75 CTests passed; continuous ReadN shipping test passes 3/3 with 31 checks, and the isolated real Crash Bash consumer advanced from LBA35799 through LBA35987 without a recompilation miss.

## Re-confirmed 2026-08-21

Post-landing Clang build and 75/75 CTests passed; continuous ReadN shipping test passes 3/3 with 31 checks, and the isolated real Crash Bash consumer advanced from LBA35799 through LBA35987 without a recompilation miss.
