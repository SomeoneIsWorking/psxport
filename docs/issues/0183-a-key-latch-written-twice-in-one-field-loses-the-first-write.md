# 0183 — A key latch written twice in one field loses the first write

Status: closed (2026-10-27). Owner: `runtime/psx/audio/spu_beetle.cpp` (`psx::audio::spu::write`, `update`).

## Observed
Tomba! 2, free-roam Start leg (`psx-tomba/scratch/player-bugs-2/keyoff_trace.py`, `PSXPORT_DEBUG=spu`). Within one
field libsnd's flush writes `KOFF 18C=0008` and then `KOFF 18C=0000`, and `KON 18A=0001` and then `KON 18A=0000`.
Voice 3 is never released and voice 16 is never keyed. The game's all-voice key-off (`KOFF 18C=FFFF 18E=00FF`)
was erased the same way, so the oracle compare saw voices keyed on the product that the console had released.

## Cause
`SpuAudio::frame` runs the SPU once per field, and every register write before it is untimed. Beetle's `spu.c`
stores a KON/KOFF write over its latch and takes the latch on the next sample. On the console that sample runs
between two writes; on the product all writes land first, so only the last write to each latch half survives.

## Fix
A KON/KOFF write to a latch half that already holds unconsumed bits runs one SPU sample first, as does a KOFF
over bits waiting in KON. The clocks run early come off the field's own advance, so the field still produces
its sample count.

## Proof
`psx-tomba tools/oracle_compare.py --frame-step 30` (`psx-tomba/scratch/player-bugs-2/oracle3.log`): the
`game_stage` and `field` checkpoints no longer report voice 0 keyed on the product alone, and at `free_roam` the
product's sounding set drops voices 16 and 19-23 (now `[0, 1, 2, 4]` against the console's `[0, 1, 2, 3, 6]`).

All-title real-time run on psxport `8a4215c5` (40 s paced, hidden, `scratch/player-bugs-2/realtime_all.py`, load
3.2-20.1): no error lines in any title. Missed presents: Spyro, Crash, Spider-Man, Vagrant and X4 0; Crash Bash, Tomba,
Tekken 3 and CTR at most 0.13%; Toy Story 2 0.63% (its boot hold, issue 0057); C-12 1.77% (psx-c12 issue 0006).
