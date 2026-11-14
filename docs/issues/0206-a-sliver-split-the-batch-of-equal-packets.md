# 0206: a sliver split the batch of equal packets

Status: fixed. Owner: `runtime/psx/gpu/record_raster_setup.cpp`, `RecordPlanner::Builder::addTriangle`.

## Observed
Crash 3, `warp scene 0x0D` (the jet-ski sea), 4:3, `ires=0` (8x), `fps60=1`, sink 3840x2160, unpaced,
`PSXPORT_DEBUG=perf`, 15 s after the warp: the host water grid (psx-crash `crash3::WaterGrid`) made 87 draws per
present against 51 for the guest grid, with the packets byte-identical; the native stores also carry the vertices'
unrounded places. At `ires=1` the two were equal (35 and 36), so the extra splits come from scale.

## Cause
`addTriangle` marked a sliver (a triangle whose places give it area where its words give none) as writing
`nativeReach(covered, s)`, the native pixels its quads reach. At 1x it writes none, so the next semi-transparent op
over a neighbouring cell found those pixels written in its batch and started a new one. The places only move
vertices, but they decided where batches split.

## Fix
A sliver's reach is kept apart from the op's writes (`opReaches_`): its depth group still counts it (issue 0199's
refused fix, letting slivers join groups, changed Spyro's picture), its batch does not. Batches now split where the
1x walk says at every S.

## Proof
Same tree, two unpaced runs per build, run-to-run spread 0 on p50 and p99, scene 0x0D at 4:3, 8x:

| build | draws/present p50 / p99 | present Minsn/frame p50 / p99 |
|---|---|---|
| native grid, before | 87 / 124 | 10.49 / 19.14 |
| native grid, after | 35 / 58 | 8.19 / 16.65 |
| guest grid, before | 51 / 87 | 9.04 / 17.56 |
| guest grid, after | 35 / 58 | 8.06 / 16.52 |

Picture, from one saved 0x0D state 60 frames on: 945 of 7.9 M pixels differ, all in the horizon band where far sea
cells are slivers; against the 1x shot of the same frame, the fixed build is nearer on 649 of them, the old on 296.
A later read in the sliver's batch sees the snapshot without it, as the same pixel at 1x does. Spyro 3 (`warp 0/1/0`)
and Tomba! 2 (`warp 2/0`) at 8x show no seams.
