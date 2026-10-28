# 0188 — The NTSC field was the broadcast 59.94 Hz, not the console's

Status: fixed (2026-10-28). Owner: `runtime/psx/frame/field_rate.h`.

## Observed
Spyro 1's intro cutscene (GS_Cutscene 14) plays its 49 s narration on one SPU voice. On the console the voice ends at
field 2937 of the cutscene and the guest frees its row through `TickActiveSoundVoices`, writing 0x7F to the sound's
slot 0x80077084. On the product the voice was 0.19% slower (83.40 against 83.55 decode units per field;
`psx-spyro/scratch/player-bugs-4/probe/cutscene_voice.log`), so it was still keyed when the cutscene ended at field
2942; `DispatchSpecialCutscene` freed it through `StopAllSoundExceptMask(0)`, which leaves the slot at 0. In Artisans
the dragon cutscene's `StopSoundVoiceByIndex(*0x80077084, 4)` then cut whatever was on voice 0.

## Cause and fix
`DISPLAY_FIELD_RATE_NTSC` was 60000/1001 Hz. A PSX non-interlaced field is 263 lines of 3413 video clocks at
53.693175 MHz, 59.817 Hz (PAL 314 x 3406 at 53.203425 MHz, 49.747 Hz), so every per-field budget (SPU samples, CPU
clocks, pacing) was short: 736 SPU samples per field against the console's 737.25. Both rates are now built from the
video clock and the line timing; the SPU mix buffer's bound follows from the PAL rate.

## Shown
`cutscene_fields.py`: the narration row is now active for 1469 product frames (2938 fields) against the console's 2937
fields, and the slot reads 0x7F at `playing` on both cores (`slot_trace2.log`).

Paced, 40 s each, host load 0.6-1.7, every picker product entered with `pick` (`scratch/player-bugs-4/paced2/summary.txt`):
nine products present 2382-2393 intervals with p99 17.0 ms and 0-1 missed (Tomba! 2 and Crash Bash p99 33.5 ms on
their two-field screens, 0-1 missed). Toy Story 2 misses 4 with one 501 ms interval: the boot load before its first
movie (guest call 0x8007BC74 spans 27 host turns while the sink skips 14 fields), not the field rate.

## Gaps
Field parity is still not modelled: an interlaced mode's fields alternate 262 and 263 lines.
