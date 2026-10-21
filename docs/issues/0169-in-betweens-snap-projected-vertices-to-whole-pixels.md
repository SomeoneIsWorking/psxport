# 0169: in-betweens snapped every projected vertex to a whole native pixel

Status: fixed for the flag intro; partial for the producers under Open. Owner: `runtime/psx/cpu/gte.h` `Gte::projectedPlace`, `runtime/psx/present/state_producer.cpp`
`PrimitiveList`, each title producer that projects (psx-spyro sky, moby emitter, terrain passes),
`runtime/psx/gpu/record_raster_setup.cpp` `scaledCoordinate`.

## Observed
Spyro 1 flag intro (psx-spyro issue 0196), fps60, 16:9, 4x internal resolution: the sky in the margins,
the near white peak at the right edge and the far terrain changed only between an in-between and its real
present, never between a real present and the next in-between (`parity.py`: 0 against 38 on those blocks).

## Cause
`composeFrame` placed a producer's polygons where the re-run drawer's packets put them: the GTE's RTPS output,
truncated to whole native pixels. A sky turning about one pixel per logic frame has no whole pixel halfway, so
its in-between drew on one neighbour's pixel at any internal resolution.

## Fix
The first fix (f8575299) matched each polygon vertex to an unrounded projection by its packed SXY value. Review
rejected it: identity guessed from a value, which two unrelated points can share. It is replaced:

- The GTE keeps the unrounded place of each SXY FIFO entry (`GteRegs::places`, pushed with the FIFO by Beetle's
  `PGXP_pushSXYZ2f`); `Gte::projectedPlace(i)` reads it.
- A producer declares each polygon vertex's place by packet address and vertex index
  (`PrimitiveSink::place`), carrying the place beside the word through its own slots and arithmetic. The packet
  walker stamps `sourceAddress`, and `PrimitiveList` gives a polygon from a packet with declared places its
  fraction in `subX`/`subY`. `placed` reads a declaration back for a producer that cuts a packet it wrote.
- Spyro declares places in the sky (`Cyclorama`), the moby emitters (`ActorEmitter`, `LitActorEmitter`) and every
  terrain pass (detail, translucent, far, coarse and fine split pieces with their grid pulls, crack triangles,
  and the re-split of oversized pieces through midpoints of the declared places).
- `scaledCoordinate` truncates the scaled fraction as the GTE truncates, so 1x is unchanged for projected
  vertices. It also truncates a fraction a title set with `placeVertex` (Spyro's title sprites and screen
  overlays), which rounded before: at 1x a half-pixel in-between position now draws on the lower pixel.

`docs/presentation.md` (Sub-pixel place).

## Proof
psx-spyro `scratch/judder-boot/{before4,fix4,tr4}/s0600`, stepped from boot, 40 presents from product frame 600 at
`PSXPORT_IRES=4`. Worst even/odd change ratio on a 16-pixel block: 38:1 before, 1.9:1 with the value-keyed match,
1.9:1 with declared places (the same blocks, x=32 y=112). With places from the sky alone it was 4.4:1 along the
sign moby and 3.0:1 on the right-hand peak, which the moby and terrain declarations closed. In-betweens within 15%
of one neighbour on a moving block: 15-29 of ~260 per in-between, as with the value-keyed match. A 4x gameplay
turn in Artisans (`wk4/walk`) shows no seams in the near split ground.

## Open
Spyro 1 producers that project but declare no places stay on whole native pixels above 1x: the paired actor
(Spyro, with its water cut), the drop and moby shadows, particles, glows, the sprite queue and portals. Each needs
its declarations at the point it copies a projected word into a packet, as the moby emitter does.
