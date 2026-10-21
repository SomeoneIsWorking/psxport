# 0169: in-betweens snapped every projected vertex to a whole native pixel

Status: fixed for polygons, lines and sprites, the flag intro, the title screen and every Spyro 1 producer below.
Owner: `PrimitiveList`, each title producer that projects (psx-spyro sky, moby emitter, terrain
passes and the gameplay producers below), `runtime/psx/gpu/record_raster_setup.cpp` `scaledCoordinate` and `addLine`.

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

## Spyro 1 gameplay producers (2026-10-21)
The paired actor (Spyro, with its water cut), the sprite queue, particles, the drop and moby shadows, the glow
fans and the portals now declare their places where they copy a projected word into a packet. Corners a drawer
offsets from a projected point by a genuine whole-pixel step (moby shadow nudges, the near portal outline's push,
particle dots) take `placeOffset`; glow rims and scaled particle quads take their unrounded offsets (below); corners no projection made (the portal fill's screen triangles) take
`wholePlace`; crossings the drawer computes (the water cut, the portal clip) are recomputed on the unrounded
places. psxport `686de8d2`, `1e945b7a`, `e0355be4`; psx-spyro issue 0196 has the per-producer proof.

## Review of pass 3 (2026-10-21)
- The places were fractions clamped into the vertex's own pixel, which a corner offset by a scaled size cannot
  respect: a glow rim is the centre plus `size / depth` times a rim vector, and the guest truncates both, so the
  unrounded rim lies up to two pixels from the guest's. `placeOffset` carried only the centre's fraction, so glow
  rims and scaled particle quads changed size in whole native pixels (8-pixel steps at 8x). `subX`/`subY` now hold
  the place's offset from the whole pixel, within two pixels either way (`PrimitiveList::emit`; a place farther off
  is a word the GPU wrapped, and keeps the whole pixel). Native scale keeps the guest's pixel
  (`record_raster_setup.cpp:placeOffset`), so 1x is unchanged: psx-spyro `scratch/judder-pass3/r2-x1-{head,new}`,
  `warp 10` turning, 20 presents pixel-identical to the previous build at 1x. Spyro 1's glow rims and scaled quads
  declare the offset before the guest truncates it (psx-spyro issue 0196).
- Lines and dots stepped on whole native pixels: `PrimitiveList::emit` applied places to polygons only and
  `addLine` drew each native pixel of the walk on the native grid. `emit` now applies places to lines, and `addLine`
  draws each pixel of the walk at the offset interpolated between the two ends' places (still one quad per native
  pixel, so the CPU cost does not scale with the resolution).
- `wholePlace` decoded a vertex word as two int16 halves; the GPU and `PrimitiveList` read it as 11-bit signed
  (`Gp0Command::vertexPos`), so a coordinate of 1024 or more disagreed. `wholePlace` now reads the word through
  `Gp0Command::vertexPos`.
- `composeFrame` built a fresh `PrimitiveList` per object per present, so declared places allocated every present.
  It renders into a caller-owned list (`FramePresenter::renderScratch_`) that `clear` empties without freeing. The
  composer's other per-present containers predate this issue: issue 0173.
- The title sprites were listed here as stepping whole; they do not. Spyro 1's `TitleSprite` (POLY_FT4 packets)
  places its in-between corners with fractions through `placeVertex`, and psx-spyro 0196 measured the logo's lowest
  edge moving 4..7 rows per present at 4x.

## Sprites and the flame (psx-spyro 0196 pass 4, 2026-10-21)
- Sprites (GP0 0x60-0x7F): `PrimitiveList::emit` now gives a sprite's origin its declared place like any vertex, and
  `RecordPlanner::Builder::addSprite` moves the sprite's quads by `placeOffset` while `record.frag` takes the texel
  of the native pixel the shifted fragment shows (`kOpShiftX`/`kOpShiftY`), so the image slides whole without
  resampling. Native scale has no offset, so 1x is unchanged; the native pixels a moved quad reaches are marked
  written (`nativeReach`). No producer in this workspace declares a sprite place yet, so no picture changes: psx-spyro
  `scratch/judder-pass4/e-x` against `c-x4` (`warp 10`, `gate-teleport 0 0`, 8x), 4 presents, 0 pixels differ. The
  first title that needs it is one whose producer re-runs a sprite drawer with a moved origin (Tomba 2's tile grid
  `TileGridLayer::emitGrid` steps whole pixels at its in-between scroll).
- Spyro 1's flame `0x80058D64` now has a producer that declares its tip and ribbon places (psx-spyro 0196).
