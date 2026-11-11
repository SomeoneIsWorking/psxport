# 0199 — Present-phase CPU work grows with ires

Status: closed, not a defect. Above 1x the present draws geometry that has no pixel at 1x, and its depth groups
and the second gpu.c walk cost per primitive, not per pixel. No loop runs per scaled pixel or row on the CPU.
Owner: the record planner (`gpu/record_raster_setup.cpp`) and `render/record_rasterizer.cpp`.

## Observed

Spyro 1, one binary, the same unpaced run at three internal resolutions. The CPU must not scale with ires
(`perf-units`); the present phase and the GPU draw, bind and upload counts do. Same 1706 logic frames and 3023
record presents in every run; game-logic and replay are flat, so the growth is the present's own.

| ires | cpu present total (Minsn) | present p99 (Minsn/frame) | gpu draws | gpu binds | gpu upload (KiB) | gpu pixels (Mpx) |
|---|---|---|---|---|---|---|
| 1 | 167360.3 | 142.606 | 1015668 | 1028177 | 2042588.2 | 6586.2 |
| 2 | 172520.4 | 148.898 | 1176372 | 1189053 | 2138231.2 | 27151.8 |
| 8 (ires 0, auto) | 177032.9 | 153.092 | 1389514 | 1402383 | 2237808.8 | 445215.2 |

Those runs also dumped 8 presents (`preseq`) inside the present phase. At 8x a dump converts 10.5 Mpx on the CPU
(`RenderState::recordShot`), about 580 Minsn each: that is the 8x `max 725 Minsn/frame` and 2.5 points of the 5.8%.
A dump is a debug capture and scales with pixels by nature. Meter runs leave it out.

## Recipe

`psx-spyro/scratch/ires0199/run.py <name> 0/1/0 0 up left` with `NODUMP=1 EXTRA_DEBUG=playcheck` and
`IRES=1|2|0`: boot 1200 steps, `warp 0/1/0`, 900 steps, hold up and left, 42 steps; `PSXPORT_NOPACE=1`,
`PSXPORT_FPS60=1`, `PSXPORT_PRESENT_SINK=3840x2160`, `PSXPORT_DEBUG=presentseq,compose,motion,perf,playcheck`
(1694 logic frames, 2999 presents). Add `record_plan` for per-record ops, quads, batches and depth groups.

HEAD `b4457f10`, two runs a side, `perf_compare.py`:

| line | 1x | 8x | delta | spread |
|---|---|---|---|---|
| cpu present (Minsn) | 165825.6 | 173221.8 | +4.46% | 0.002% / 0.014% |
| gpu draws | 999650 | 1370410 | +37.1% | 0 |
| gpu binds | 1011861 | 1382981 | +36.7% | 0 |
| gpu render passes | 12217 | 12577 | +2.9% | 0 |
| gpu triangles (ktri) | 42156.4 | 56364.8 | +33.7% | 0 |
| gpu upload (KiB) | 2022095 | 2215111 | +9.5% | 0 |

Per record (`record_plan`, 3023 plans): ops 9.81M → 10.10M, quads 14.13M → 18.95M, batches 6258 → 6439, depth groups
177553 → 266695.

## Cause

Above 1x, vertex places give area to geometry that has none at 1x, and the planner draws it:

- **Slivers.** A triangle whose words give no area but whose places do (`addTriangle`, no native walk) draws above 1x
  and writes the native pixels its quads reach. About 48 per record at 8x. It covers native pixels of the polygons it
  lies between, so `RecordPlanner::Builder::depthGroup` closes the open group at nearly every one: +29 groups per
  record, two draws per group per plane. That is 35 of the 37 points of draw growth, and the driver's draw and bind
  work in the present phase. A sliver drawn there is depth tested against what it overlaps.
- **Rows only at S.** A triangle part whose two vertices share a row at 1x has no rows there. With places it has a
  few rows at S and one quad more: quads +34%. An op that reaches a plane only at S (101k such op-planes at 1x, 844k
  at 8x) can split a depth run there.
- **The walk at S.** Every triangle needs its 1x walk, which decides what it writes, and a walk at S to draw. At 1x
  they are one walk. Above 1x it is one more `walkTriangle` per triangle, about 2.4 G of the run's 8x growth
  (`perf record -e instructions:u`).

All three are per primitive and bounded per triangle (one extra walk, at most two parts per plane, one group split
per sliver). They grow from 2x to 8x only because a finer scale truncates fewer places to the whole pixel: 2x draws
1159312, 8x 1370410.

## Fixes tried and refused

- **A sliver joins the open group** (`depthGroup` skips its reach). 8x: draws −25.9% (1014868, +1.5% over 1x),
  present −1.5% (+2.9% over 1x). The picture changes: 160-190 scaled pixels a frame, a one-row seam of the wall's
  sliver painted in slot order over the nearer grass at the Stone Hill wall foot
  (`psx-spyro/scratch/ires0199/pic8x-p3-zoom10.png`: HEAD, fix, diff mask).
- **Held slivers drawn as one group** after the tested polygons around them (flushed at the next untested op or
  batch end). 8x: draws +7.5% over 1x, and 230-540 pixels a frame change.
- **A sliver's reach per native row** instead of its part's box. The picture is identical to HEAD, and draws drop
  only to 1355660: a sliver really covers its neighbours' native pixels.

Dropping slivers or S-only rows leaves seams (`47b972ef`). So every remaining option either changes the picture or
leaves the cost as it is.
