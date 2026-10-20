# 0166: a movie's Start skip is not reachable from the control channel

Status: open. Owner: the titles' movie steps (Tomba! 2 `game/core/frame/boot_cards.cpp` `BootCards::stepMovie`,
Toy Story 2 `game/fmv/movie_player.cpp`), against `Fmv::pace`'s contract in `runtime/psx/movie/native_fmv.cpp`.

## Observed
Tomba! 2, hidden window: `tap start 6` on the control channel 60 frames into `MOVIE/LOGO.STR` does not skip
it; the movie plays all 255 frames (`psx-tomba/scratch/b2fix/movie-skip.log`).

## Cause
`Fmv::pace` reads the skip from `pad.buttons`, which its contract says the frame's pad service already
resolved from host, replay and control input. The movie steps call only `Pad::pollHostInput`, which sets
`buttons` from the window and does nothing without one; the control channel's tap and a pad replay are
applied by the pad service, which does not run while a movie plays.

## Fix
The movie step runs the title's pad service for the field instead of `pollHostInput`, so host, replay and
control input reach the skip alike.
