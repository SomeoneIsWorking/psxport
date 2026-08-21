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

The shipping-path hermetic test supplies both discriminators. The consumption-owned model emitted
no following `INT1`; the later BFRD-owned model emitted one immediately. The controller instead
needs a drive deadline independent of both FIFO consumption and BFRD traffic.

## Resolution

`Timing::guestInstructionTicks` now owns deterministic guest instruction-time. Emitted and interpreted guest
execution advance the same counter; ReadN schedules its first sector and every following sector at
nominal 451,584-tick (1x) or 225,792-tick (Setmode bit 0x80, 2x) thresholds. BFRD only exposes or swaps a sector that the
drive event already made ready. Pause/Stop cancels the owned deadline.

The shipping test passes 5/5 with 59 checks, including too-early, exactly-due, partial-tail, Pause,
full-drain and both-speed answers. Real Crash Bash returns from its 189-sector start with all 189
still pending, then services LBA35799..35987 at +225,792-tick deadlines despite every sector
leaving 280 bytes unread, prints `done loading`, and reaches the next recompilation boundary. Issue
0007 records that one tick per instruction is not a cycle-accurate physical drive model.
