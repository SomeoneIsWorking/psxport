# 0204: a read completion behind a Pause was taken by the guest's CD handler

Status: fixed. Owner: `runtime/psx/cd/cdc_native.cpp:CdController::serviceReadCompletions`, with
`runtime/psx/cd/cd_ready_delivery.cpp:OwedCompletionHold` held by `Hle::irqPoll`.

## Observed
Spyro 3, `warp 0/1/0` with the loading picture's count put at 0xD at count 0 by a REPL write (psx-spyro issue 0248, evidence in psx-spyro
`scratch/cd0248/head.trace`): the 18-sector `CdRead` that loader stage 6 issues right after libcd's `CdlPause`
(`0x8006143C`) never reached its ready callback (`19 of 19 delivered`, no 20th), and stage 7 waited 75 updates
(steps 1024-1099) for `CDLoadTime`'s timeout to re-issue the read.

## Cause
`CdController::serviceReadCompletions` queued the native `CdRead`'s owed INT1 behind the Pause's INT3, and once the
guest acknowledged that INT3 the INT1 was current inside the guest's own interrupt dispatch (libetc's loop at
`0x8005C8A8` calling libcd's CD handler). libcd acknowledged it with no read state of its own (the read is native),
so `deliverReadyCompletion`, which runs only at the start of a dispatch, never saw it. Controller trace: ack INT3,
ack INT1, ack INT2, all by the guest. Beetle raises an async INT1 only with no interrupt pending (`IRQBuffer`
clear), so the INT1 could never have arrived between a command's INT3 and INT2 on the drive.

## Resolution
An owed read completion becomes current only with an empty response queue and outside an interrupt dispatch:
`Hle::irqPoll` holds `OwedCompletionHold`, and its outermost release services the drive, so a completion that fell
due during the dispatch raises its edge for the next one, where the ready delivery takes it first. Titles affected:
those binding `GuestInterruptLibcd` with `perStockRead` (Spyro 2 and 3); every title now services the drive once
at the end of each `irqPoll`.

## Shown
Same reproduction (`fix2.trace`): the guest acknowledges the Pause's INT3 and INT2, the INT1 is
announced after the dispatch and delivered by the framework in the same update (`20 of 20 delivered`); stage 7
lasts one update, gameplay at step 1050 instead of 1124. Unskipped warps land identically to psx-spyro issue 0248's
traces: Spyro 3 (`s3.trace` = `loadpic2/s3final.trace`) and Spyro 2 (`s2.trace` = `loadpic2/s2after.trace`), line
for line. Spyro 1 (`warp 0/0/0` then `0/1/0`) and Tomba 2 area loads (`warp 1/0`, `0/0`, `PSXPORT_AUTO_SKIP=1`) match a
build without the change step for step in position and area (`s1head.trace`, `t2head.trace`). All runs unpaced.
