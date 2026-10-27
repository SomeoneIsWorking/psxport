# 0180 — Card read on a failed open logged as an error

Status: closed (2026-10-27). Owner: `runtime/psx/platform/memcard.cpp` (`file_read`, `file_write`).

## Observed
Tekken 3 logs `[card:error] read on fd -1 which is not an open card file — returning -1` at boot on a fresh card.

## Cause
With `PSXPORT_DEBUG=card`: `open 'bu00:BASLUS-00402TEKKEN-3' mode=1 -> FAIL`, then B0:0x34 read and B0:0x36 close on
the returned fd -1. The guest reads its save without checking the open, and the BIOS answers -1 for a bad descriptor,
which is what `file_read` returns. The error level and the comment claiming no guest reaches this path were wrong.

## Fix
The bad-descriptor read and write log at debug level; the comment states the case.

## Proof
`psx-tekken-3/scratch/player-bugs/card2`: the same open/read/close sequence, no error line.
