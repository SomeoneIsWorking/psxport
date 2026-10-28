# 0190 — GetlocL answered zero, so a saved stream resumed at 00:00:00

Status: fixed (2026-10-28). Owner: `runtime/psx/cd/cdc_native.cpp` (`CdController::getlocL`), answered for stock
libcd by `runtime/psx/cd/stock_cd_response.cpp` (`beginStockCommand`).

## Observed
C-12, NEW GAME, X to skip the briefing: `[cd:error] setloc 00:00:00 is inside the lead-in` and the same for the
implicit Setloc of the ReadS that follows (psx-c12 issue 0007).

## Cause
C-12's sound queue (`FUN_800577AC`) saves the playing XA stream with GetlocL (command 0x10, case 7) and resumes it
after the briefing with Setloc/ReadS on the saved header (case 9). Neither the controller nor the stock command path
answered GetlocL: the stock result was zero-filled, so the saved position was 00:00:00.

## Fix
The controller latches the header and subheader of every sector the head reads, as Beetle's `HeaderBuf` does. Where
`vendor/beetle-psx/mednafen/psx/cdc.c` clears `HeaderBufValid`, the controller drops the latch: ReadN/ReadS, SeekP,
Play, Stop and ReadTOC. Pause, SeekL and Reset (0x0A) keep it, as Beetle does; Beetle's other clears (the HCLRCTL bit-7
soft reset, disc change, CD-DA play reads) have no counterpart in the controller. GetlocL returns those eight bytes,
or error 0x80 with no latched sector, on the register path and on the stock path. The latch is saved with the CD
section (`runtime/psx/state/device_bus.cpp`, bus layout 6; earlier states are refused), so a restored state answers
from the saved machine's last sector.

## Shown
Same C-12 route with `PSXPORT_DEBUG=cdcmd`: GetlocL, then at the skip `Pause`, `Setloc 23:03:62` and `ReadS 23:03:62`
(the music started at 23:03:54), no `cd:error`, and the first mission plays
(`psx-c12/scratch/player-bugs-4/briefing2/`).

Savestate round trip (`psx-c12/scratch/player-bugs-4/state1/`): `state save` at the main menu, play into the briefing,
`state load`, then NEW GAME and skip again. The restored run issues GetlocL, plays the briefing from 148549 and at
the skip resumes `ReadS 23:03:62`, as the unbroken run does; no `cd:error`.
