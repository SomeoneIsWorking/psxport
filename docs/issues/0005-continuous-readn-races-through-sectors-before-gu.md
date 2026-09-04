---
id: 5
title: Continuous ReadN races through sectors before guest code can return
status: resolved
symptom: Toy Story 2 advances LBA16 through LBA21179 in under 20 seconds without presenting a frame; Crash Bash completes a 189-sector async read before its start call returns; Vagrant queues Pause behind an already-pending next INT1
tags: cdc,cdrom,readn,pacing,timing,toystory2,crashbash,vagrant
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

The prior FIFO/drive separation made `write_request_register()` call `announce_following_sector()` synchronously. BFRD is the guest data-request latch, not a drive clock, so every callback/DMA cycle manufactured the next INT1 before ordinary guest code could return.

## Resolution

Per-Game `Timing::guestInstructionTicks` now owns deterministic instruction-time deadlines. The CPU
executor accounts for the guest instructions actually executed.
ReadN schedules the first and every following sector at nominal 451,584-tick 1x or 225,792-tick
Setmode bit 0x80/2x thresholds. BFRD is passive, and Pause/Stop cancels the outstanding event. The clock seam is injectable,
so the shipping CDC test exercises deadline-1, exactly-due and cancelled outcomes without sleeps.

The controller also reports the measured `CdlStatRead` bit: INT1 is 0x22 while reading, Pause ACK is
0x22 and completion is 0x02. Without that bit Vagrant Story's libds VSync state machine never changed
Busy to Idle, so its queued Pause could not arbitrate before the next sector.

## Consumer verification

- Toy Story 2's baseline advances 21,164 contiguous sectors in one phase. The deterministic build
  performs 358 sectors over 11 ReadN phases; its longest phase is 209, with stock ACK/DMA preserved,
  and reaches a later BITS/MEMORY path.
- Crash Bash returns from a 189-sector start before its first INT1, then services LBA35799..35987 at
  +225,792-tick deadlines, prints `done loading`, and reaches 0x80092BDC. Issue 0006 records a later
  transient-state mismatch; this resolution claims drive pacing, not interrupt return ordering.
- Vagrant Story completes its 17-sector read and dispatches Pause after callback 17. It completes
  five ReadN transfers: four WAVE loads plus TITLE.PRG at LBA256000..256270. The sixth
  `DsEndReadySystem` call is initialization, not another read. TITLE.PRG then exposes the later
  overlay address 0x80071334.

Issue 0007 records why these deterministic thresholds do not yet establish physical drive-rate or
cycle accuracy.
