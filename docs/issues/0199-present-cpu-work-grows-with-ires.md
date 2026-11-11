# 0199 — Present-phase CPU work grows with ires

Status: open. Owner: the record present path (`render/record_rasterizer.*`, `render/record_present.cpp`) and what
feeds it draws.

## Observed

Spyro 1, one binary, the same unpaced run at three internal resolutions. The CPU must not scale with ires
(`perf-units`); the present phase and the GPU draw, bind and upload counts do. Same 1706 logic frames and 3023
record presents in every run; game-logic and replay are flat, so the growth is the present's own.

| ires | cpu present total (Minsn) | present p99 (Minsn/frame) | gpu draws | gpu binds | gpu upload (KiB) | gpu pixels (Mpx) |
|---|---|---|---|---|---|---|
| 1 | 167360.3 | 142.606 | 1015668 | 1028177 | 2042588.2 | 6586.2 |
| 2 | 172520.4 | 148.898 | 1176372 | 1189053 | 2138231.2 | 27151.8 |
| 8 (ires 0, auto) | 177032.9 | 153.092 | 1389514 | 1402383 | 2237808.8 | 445215.2 |

1x to 8x: present +5.8% CPU instructions, draws +36.8%, upload +9.6%. Run-to-run spread of the present total on one
build is 0.041% (three runs), so this is not noise. Pixels scale with ires squared, as they should.

## Recipe

`psx-spyro/scratch/perfunit/run.py <name> 0/1/0 0 up left` with `EXTRA_DEBUG=playcheck` and `IRES=1|2|0`: boot 1200
steps, `warp 0/1/0`, 900 steps, hold up and left, 30 steps, 8 present dumps, 12 steps; `PSXPORT_NOPACE=1`,
`PSXPORT_FPS60=1`, `PSXPORT_PRESENT_SINK=3840x2160`, `PSXPORT_DEBUG=presentseq,compose,motion,perf,playcheck`.
Compare with `perf_compare.py ires1.log --vs A.log`.

## Next

Find which record ops split into more draws at higher ires (the draw count is the CPU cost's likely carrier) with
`perfframe` per-present counts at ires 1 and 8 on one present.
