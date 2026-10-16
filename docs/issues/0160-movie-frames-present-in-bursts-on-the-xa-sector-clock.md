# 0160: movie frames present in bursts on the XA sector clock

Status: open

## Observed
Toy Story 2 real-window play check (2026-10-16): the four intro movies account for 473 of the run's 476 missed
vsyncs (after 0158, judged against each frame's held fields). Movie frames arrive in bursts: two ~16.7 ms apart,
then one ~70-75 ms later (15 fps movies: ~165 ms), repeating. Tomba! 2's 50 ms intervals inside `MOVIE/OP.STR`
(0158) are the same pattern.

## Cause
`runtime/psx/movie/native_fmv.cpp:Fmv::pace` waits for `media_frames_`, the audio decoded so far, which advances one
XA sector (53 ms of 37.8 kHz stereo) at a time, so the video frames between two audio sectors share one deadline:
they present back to back and the next frame waits for the next sector.

## Gap
Each video frame needs its own deadline on the stream's clock, the sector position of its last chunk at the stream's
sector rate (derivable from the XA interleave: one sector's audio spans the sectors to the next XA sector). That
changes every title's movie timing and needs a real-window check of each title's movies; not made here.
