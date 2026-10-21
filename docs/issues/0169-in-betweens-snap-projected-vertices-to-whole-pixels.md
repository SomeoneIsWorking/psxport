# 0169: in-betweens snapped every projected vertex to a whole native pixel

Status: fixed. Owner: `runtime/psx/present/frame_composer.cpp` `composeFrame`,
`runtime/psx/present/precise_projection.*`, `runtime/psx/gpu/record_raster_setup.cpp` `scaledCoordinate`.

## Observed
Spyro 1 flag intro (psx-spyro issue 0196), fps60, 16:9, 4x internal resolution: the sky in the margins,
the near white peak at the right edge and the far terrain changed only between an in-between and its real
present, never between a real present and the next in-between (`parity.py`: 0 against 38 on those blocks).

## Cause
`composeFrame` placed a producer's polygons where the re-run drawer's packets put them: the GTE's RTPS output,
truncated to whole native pixels. A sky turning about one pixel per logic frame has no whole pixel halfway, so
its in-between drew on one neighbour's pixel at any internal resolution.

## Fix
While a frame is composed the GTE hands each projected point, unrounded, to `PreciseProjection` (Beetle's
projection push, through `GteRegs::preciseSink`), keyed by the packed SXY it rounded to. After each render a
polygon vertex whose word was projected keeps its fraction in `subX`/`subY`; points that round to one word share
the first one's place, so polygons sharing a vertex stay joined. `scaledCoordinate` truncates the scaled fraction
as the GTE truncates, so 1x is unchanged. Real presents are composed the same way, so a still object does not
shift between presents. `docs/presentation.md` (Sub-pixel place).

## Proof
psx-spyro `scratch/judder-boot/{before4,fix4}/s0600`, stepped from boot, 40 presents from product frame 600 at
`PSXPORT_IRES=4`: in-betweens within 15% of one neighbour on a moving 16-pixel block, 30.3% before, 4.2% after
(with psx-spyro's fine camera angles); the margin blocks alternate 0/38 before and at most 1.9:1 after. At 1x
every real present is pixel-identical to the previous build (20 of 20, max difference 0). All eleven title
products build with Clang.
