---
id: 4
title: Continuous ReadN stalls when a consumer leaves raw-sector tail bytes unread
status: resolved
symptom: Crash Bash receives LBA35799 payload but no following INT1 or LBA35800 and spins in file-read synchronization
tags: cdc,cdrom,readn,dma,crashbash,buffering
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

The synchronous CDC model made `sector_consumed()` the owner of continuous ReadN advancement and its
next `INT1`. That couples the drive's sector-arrival state to complete drainage of the
software-visible BFRD data FIFO. Real controller state has separate drive/sector and DMA buffers:
the drive can receive and announce a following sector while bytes from the accepted sector remain in
the DMA FIFO.

Crash Bash exposes the defect at whole-sector LBA 35799. It DMA-reads 3 header/subheader words and
512 payload words, intentionally leaving the 280-byte EDC/ECC tail unread (`data_rd=2060`,
`data_n=2340`). The old model therefore never called `sector_consumed()`, never raised the following
`INT1`, and left the guest spinning in its file-read synchronization path.

## What was tried / dead ends

Advancing after a magic 2060-byte threshold would encode one consumer's transfer shape and break
other sector modes. Advancing on interrupt acknowledgment was already disproven by streaming readers
that drive BFRD without using acknowledgment as the data-consumption boundary. The controller needs
separate drive-side following-sector availability and software FIFO-consumption state.

The shipping-path hermetic test supplies the discriminator. Before the fix its partial-FIFO case
failed because no following `INT1` was pending; its stopped-controller case passed, ruling out
unconditional event generation.

## Resolution

### Resolution (2026-08-21)
Separated drive-side following-sector availability from BFRD data-FIFO consumption in cdc_native.cpp. Accepting a sector now records and emits exactly one following INT1 without changing the current FIFO; draining only empties/rearms BFRD, and a later real service request installs the announced sector. The pre-fix shipping-path test failed both event discriminators (IRQ type 0 instead of 1); final test passes 3/3 and 31 checks, including an actual Pause INT3/INT2 opposite case, and the prior split-DMA suite remains 3/3 and 535. Real Crash Bash, built with PSXPORT_DIR pointing at the isolated worktree, DMA-read LBA35799 as 3+512 words while leaving 280 bytes, observed pending E1/ack, wrote BFRD 00 then 80, loaded LBA35800, and completed the next 3+512-word DMA. It continued through LBA35987: 189 sectors per pass over 21 passes, with no Cant-find or recomp-miss. Repeated high-level loading is the next distinct consumer boundary, not a failure of continuous delivery; evidence crashbash/scratch/logs/cdc-continuous-consumer.log.
