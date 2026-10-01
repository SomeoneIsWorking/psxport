---
id: 145
title: A guest spinning on RAM never received a CD interrupt, because a Lightrec segment ended only at the host-turn deadline and device time advanced only on a register access
symptom: Toy Story 2's FMV owed a CD sector (`stream_active=1 setloc_lba=12718`, `I_STAT=0x004`) and delivered 0 callbacks for the whole 564,492-cycle turn
state_items: S002
tags: cd,interrupt,executor,segment-budget,device-clock,hle,dma-callback,toystory2,root-cause
created: 2026-10-01
updated: 2026-10-01
---

## Answer

**Device time (the CDC drive and command deadlines) is advanced only when guest code touches a device
register (`commitDeviceClock`) or when a segment ends (`accountExecutedInstructions`). A segment's
cycle budget was capped only by `hostTurnTicksUntilDue`.** A guest that waits for the CD by polling
a RAM word (Toy Story 2's STR pop, `0x800D6DC8`, a 32,768-try loop on a ring entry) touches no
register, so a CD deadline that fell inside the turn was never reached before the turn's budget
ended, and the data-ready interrupt was never raised in time for the same turn's `irqPoll`.

Fix: `Timing::ticksUntilDeviceEvent()` reports the ticks to the earliest armed CDC deadline
(`cdc_next_deadline_ticks`), and the executor caps each segment at it, exactly as it already did for the
host-turn deadline. Proven both ways by `tests/test_cd_deadline_segment.cpp`: with the cap disabled
the positive test fails at `result.returned()`; the control with no armed deadline runs its whole 4M
budget and delivers nothing.

Not covered: the SIO acknowledge deadline is not part of the cap.

## Two seams this exposed, both needed to reach the end of the movie

1. `PlatformHlePlan::dmaCallbackTable` (new). libapi's DMA IRQ dispatcher (`0x800890C4`) calls
   `[0x8009FD60 + 4*ch]`. The framework stands in for the BIOS DMA handler and acknowledges the
   channel flag first, so a direct runtime whose libapi keeps the table in guest RAM must let the
   framework read it; before, a direct runtime could only use the native `DmaCallbackRegistry`.
   Without the table the libcd data-end callback `0x80093E88` (which writes ring state 2) was never
   called. Tests: `test_direct_dma_callbacks.cpp` (declared table dispatches the guest word; an empty
   slot delivers nothing and ignores the registry).
2. `PlatformHlePlan` now also carries `cdGetSectorAddress` and the GPU timeout pair, so a typed
   runtime installs the same `CdGetSector`/timeout/check natives the legacy adapter did.

## Evidence

Toy Story 2 (`SLUS_008.93`), headless: 6 of 6 owed CD completions delivered; the four intro movies
return after 194, 41, 73 and 69 display fields. See toystory2 issue 0032.
