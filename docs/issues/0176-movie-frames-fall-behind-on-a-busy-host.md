# 0176: movie frames fall behind on a busy host

Status: fixed

## Observed
Tomba! 2 boot walk at real speed (8x on a 3840x2160 sink, fps60, hidden window, display simulated at 60.00 Hz from
`presentseq`; psx-tomba `scratch/judder-check`). Once per run or so a movie froze for about a quarter second, then
ran fast to catch up: `review-r2` (load 2.7 to 1.0) `OP.STR` held one picture for 276 ms at 76 s, 15 drops and
19 repeats; `fx-p3` (load 1.2 to 1.3) `OP.STR` ran 30 to 90 ms per frame for 1.7 s at 65 s and ended 300 ms
late, 30 repeats and 27 drops. The disc logged no slow fill in either run (worst fill 13.7 and 13.5 ms).

## Cause
`movie/mdec_device.cpp:MdecDevice::dmaOut` waited for each output burst by running the Beetle MDEC 256 clocks at a
time, up to 4096 times. Whenever the decoder parked for input it spun all 4096 runs before returning. A
temporary per-phase timer in `Fmv::step` measured decode at p50 22.0 ms of the 33.4 ms frame period (`fx-h0`), and
`perf` on `OP.STR` put 86% of the process in `MDEC_Run`, `dmaOut` and `MDEC_DMACanRead`; the IDCT was 0.7%. With
decode taking two thirds of each frame, any host slowdown (`fx-t1`: the system at 100% CPU, decode 36 to 69 ms)
put frames behind, and the lateness built up until the host recovered.

## Fix
`dmaOut` runs the decoder to its next park once (`step`), and returns short when no burst is ready, since the
decoder is waiting for input. `FrameDecoder::decodeMacroblocks` then feeds more input.

## Proof (2026-10-22)
- Every decoded movie frame is byte-identical: FNV-1a hashes of the RGBA output of all 2281 `LOGO.STR` and `OP.STR`
  frames match between the old drain (`fx-h0/hashes.txt`) and the new one (`fx-h1/hashes.txt`).
- Decode p50 2.47 ms, p99 3.88 ms (was 22.0, 27.45). Frame lateness max 1.31 ms (was 8.12).
- `fx-h1`, cold page cache, load 3.4 to 1.8 through the movies: `LOGO.STR` 1 repeat, 0 drops; `OP.STR` 4 repeats,
  1 drop (the 59.94 Hz media clock slipping against a 60.00 Hz display).
