# 0183 — A key latch written twice in one field loses the first write

Status: partial (2026-10-27). Owner: `runtime/psx/audio/spu_beetle.cpp` (`catchUp`, `takePendingKeys`, `beginField`,
`finishField`).

## Observed
Tomba! 2, free-roam Start leg (`psx-tomba/scratch/player-bugs-2/keyoff_trace.py`, `PSXPORT_DEBUG=spu`). Within one
field libsnd's flush writes `KOFF 18C=0008` and then `KOFF 18C=0000`, and `KON 18A=0001` and then `KON 18A=0000`.
Voice 3 is never released and voice 16 is never keyed. The game's all-voice key-off (`KOFF 18C=FFFF 18E=00FF`)
was erased the same way, so the oracle compare saw voices keyed on the product that the console had released.

## Cause
`SpuAudio::frame` ran the SPU once per field and every register write before it was untimed. Beetle's `spu.c`
stores a KON/KOFF write over its latch and takes the latch on the next sample. On the console that sample runs
between two writes; on the product all writes landed first, so only the last write to each latch half survived. The
same held for every other register: a start address, pitch or ADSR written after a KON in the same field was the one
the voice keyed with.

## Fix
1. `SpuAudio` plans each field's clocks at the previous boundary (`spu::beginField`). Every SPU register read or
   write and every SPU DMA first runs the SPU to the guest's emulated CPU time (`Timing::emulatedCpuTicks`, committed
   at device accesses by the executor). Fields follow each other on that clock, a guest past its field's end runs on
   into the next field (at most one field ahead), and `spu::finishField` runs what the field still owes, so each
   field still produces its cadence's samples.
2. Writes with no guest time between them still need the latch rule: a KON/KOFF write to a latch half that holds
   bits no sample has taken yet runs the SPU to its next sample first, as does a KOFF over bits waiting in KON.

`PSXPORT_DEBUG=spu` prints each KON/KOFF write's position in its field (`field+N` clocks).

## Remaining gap
Writes that carry no guest time between them are still simultaneous: a native override's writes (no guest
instructions run between them), and a guest more than one field ahead of its field. For those only the KON/KOFF latch
is protected; a KON followed by a new start address for the same voice in one native call still keys the voice with
the new address. The owner fix for that is native overrides charging guest time at their device accesses.

## Proof
Key writes are spread over the field: a 25 s Tomba run with `PSXPORT_DEBUG=spu`
(`psx-tomba/scratch/player-bugs-2/spu-timed2/run.log`) puts 8904 of 11678 KON/KOFF writes at 80-90% of their field
(the guest's own vsync-time flush), 1378 at its start and 1376 carried past its end; before, all landed at 0.

`psx-tomba tools/oracle_compare.py --frame-step 30` (`oracle5.log`): the latch rule changed which divergence is
reported first at `game_stage` (voice 0 keyed became voice 12 keyed); it did not clear the product-only voices.
`game_stage` still sounds voices 12 and 13 on the product and none on the console, and at `free_roam` the product
sounds `[0, 1, 2, 4]` against the console's `[0, 1, 2, 3, 6]`. The timed SPU leaves both sets unchanged. These are
title divergences still to trace (psx-tomba issue 0026).

All-title real-time run on psxport `8a4215c5` (40 s paced, hidden, `scratch/player-bugs-2/realtime_all.py`, load
3.2-20.1): no error lines in any title. Missed presents: Spyro, Crash, Spider-Man, Vagrant and X4 0; Crash Bash, Tomba,
Tekken 3 and CTR at most 0.13%; Toy Story 2 0.63% (its boot hold, issue 0057); C-12 1.77% (psx-c12 issue 0006). That
table hid three stalls: Tekken 3 427 ms (issue 0184, fixed), Toy Story 2 510 ms and CTR 200 ms.

All-title real-time run with the timed SPU and issue 0184 (40 s paced, hidden, `scratch/player-bugs-2/realtime_all3.log`,
load 3.3-16.7): no error lines. Missed: Spyro, Crash, X4, Tekken 3 0; Crash Bash, Tomba, Spider-Man, Vagrant at most
0.05%; CTR 0.09% (one 184 ms present, psx-crash-team-racing issue 0040); Toy Story 2 0.50% (501 ms boot hold, issue 0057);
C-12 3.98% (psx-c12 issue 0006). Worst intervals otherwise 17-78 ms; Tekken 3 24 ms.
