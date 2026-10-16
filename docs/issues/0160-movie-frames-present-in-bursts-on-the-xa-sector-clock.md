# 0160: movie frames present in bursts on the XA sector clock

Status: fixed

## Observed
Toy Story 2 real-window play check (2026-10-16): the four intro movies account for 473 of the run's 476 missed
vsyncs (after 0158, judged against each frame's held fields). Movie frames arrive in bursts: two ~16.7 ms apart,
then one ~70-75 ms later (15 fps movies: ~165 ms), repeating. Tomba! 2's 50 ms intervals inside `MOVIE/OP.STR`
(0158) are the same pattern.

## Cause
`runtime/psx/movie/native_fmv.cpp:Fmv::pace` waited for `media_frames_`, the audio decoded so far, which advances one
XA sector (53-213 ms) at a time, so the video frames between two audio sectors shared one deadline: they presented
back to back and the next frame waited for the next sector. `Fmv::heldFields` estimated the held fields from the same
clock (media time over frames so far).

## Fix
`movie_clock.*`: `MovieClock` learns the sector period (one XA sector's audio over the XA stride) and the frame slot
(sector distance between two frame ends no XA sector separates) from the stream's leading sectors; frame n is due
when its slot has been read. `MovieFieldGrid` shows each frame from the first field at or after its due time and
gives the whole fields its predecessor was held for, which `Fmv::presentFrame` reports to `GpuPerf::heldFor`;
`Fmv::heldFields` is gone. Audio reads ahead on its own cursor: the first XA sector starts the device at its delivery
time, the rest are queued one second ahead, so a host stall does not underrun the device and leave the audio behind
(a 532 ms stall in OP.STR did that with a two-sector lead). `xaSectorFormat` is the one parse of the XA coding byte.
Test: `tests/test_movie_clock.cpp` (frames due one slot apart across XA sectors for the LOGO.STR and TT.STR layouts,
25 fps holds of 2 and 3 fields that add up).

## Evidence (2026-10-16, visible vsynced window, 60 Hz display)
Per movie, intervals between its own frames (`[fmv] frame` lines; the `done` line's count adds the one interval into
the movie's first frame, which includes the hand-off from the guest):

| movie | frames | missed | longest run | audio lag first/last quarter |
|---|---|---|---|---|
| Tomba! 2 LOGO.STR | 255 | 0 | 0 | -11.1 / -10.7 ms |
| Tomba! 2 OP.STR | 2026 | 2 | 1 | 4.9 / 5.2 ms |
| TS2 ACTI.STR | 438 | 0 | 0 | 0.2 / 1.5 ms |
| TS2 DLOGO.STR | 156 | 0 | 0 | -3.4 / 2.1 ms |
| TS2 TT.STR | 301 | 0 | 0 | -1.5 / -0.2 ms |
| TS2 TRALER2.STR | 452 | 1 | 1 | 4.8 / 7.3 ms |

Whole runs: TS2 `missed=10 (0.36%) longest_miss_run=2` (was 476), Tomba! 2 `missed=6 (0.21%) longest_miss_run=1`.

## Remaining
Decoding one 320x240 frame (BS + Beetle MDEC) takes ~16 ms p50, 22 ms p99, half a 30 fps frame's budget, so a host
hiccup makes the next frame late; the remaining in-movie misses are such frames.
