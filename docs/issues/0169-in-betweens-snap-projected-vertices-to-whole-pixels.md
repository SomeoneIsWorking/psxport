# 0169: in-betweens snapped every projected vertex to a whole native pixel

Status: fixed. Every screen vertex a producer stores carries its place; Tomba 2 needs no declarations.
Owner: `runtime/psx/cpu/projected_vertex.h`, `present/emit_memory.*` (`storeVertex`, `loadVertex`, `HostMemory`
places), `present/gp0_primitive_decode.*` (`PacketView`, `PacketBuffer`), `present/packet_chains.cpp`,
`gpu/record_raster_setup.cpp` (`scaledCoordinate`, `placeOffset`, `addLine`, `addSprite`).

## Observed
Spyro 1 flag intro (psx-spyro issue 0196), fps60, 16:9, 4x: the sky in the margins, the near peak and the far
terrain changed only between an in-between and its real present (`parity.py`: 0 against 38 on those blocks).
Tomba 2 at 8x: every composed in-between drew on whole native pixels (8-pixel steps), since no Tomba producer
declared a place.

## Cause
A place reached the rasterizer only when the producer declared it (`state_producer.cpp:PrimitiveSink::place`
and its `placeCorners`/`placeOffset`/`wholePlace` variants), so a producer that declared none, or missed one
vertex, drew the GTE's truncated SXY word: whole native pixels, and a seam where a declared and an undeclared
vertex met.

## Fix
`docs/presentation.md` (Sub-pixel place). The GTE hands SXY entries out only as `gte::ProjectedVertex` (word plus
place); a body stores one with `EmitMemory::storeVertex` and reads it back with `loadVertex`. Over a render's
`HostMemory` the word keeps its place until another write covers it, so scratch slots, queued pieces and packets
carry places as long as each copy goes through `loadVertex`/`storeVertex`; a plain word copy drops the place
silently (the vertex draws on its word's pixel), and nothing enforces the path. Over guest memory only the word is
stored. A producer computes a
place only for a vertex it makes itself (clip crossings, split midpoints, pulls, scaled corners, `steppedTo`,
`nearest`) and stores it the same way. `PacketChainWalker` reads places with the words and
`decodePacketPrimitive(PacketView)` applies them to polygons, lines and sprites within two pixels of the whole
pixel. The declaration API and every per-producer declaration are deleted (psx-spyro, psx-tomba, psx-toy-story-2,
psx-crash-bash, psx-c12).

`gp0_primitive_decode.cpp:polygonVertex` copied vertex 0, applied offset included, into a flat polygon's later
vertices, and `place` left a rejected (wrapped) place's stale offset there: Spyro 1's `warp 10` portal fill showed
slivers. A later vertex now starts from no offset.

`PacketChainWalker::walk` built a 255-entry place array per bucket chain, so a Tomba state render (one walk per
OT bucket) constructed half a million places per object per present and in-betweens ran 8 ms late; the buffers are
members of the walker.

## Proof (2026-10-22, 8x on a 3840x2160 sink, fps60)
- Spyro, stepped from boot against a HEAD build: the flag intro, Spyro 1 `warp 10` turning, Spyro 2 boot and
  Spyro 3 `warp 10` are byte-identical to it, 24 of 24 presents each (Spyro's declarations already placed every
  vertex, so the same places reach the rasterizer and no crack opens).
- Tomba 2: every GT3/GT4 vertex of a composed render carries a place (none did before). The new-game intro's
  in-betweens lie between their neighbours on the moving effect blocks (`scratch/judder-check/seq-intro6`).
  Gameplay in-betweens are not composed yet (psx-tomba issue 0039), so gameplay still steps at 30 Hz.
- Real-speed intro, load 1.6: in-between lateness p50 0.18 ms, p90 0.20 ms (7.6 ms before the walker fix), 1
  simulated-display repeat in 1087 presents.

## Review fixes (2026-10-22)
- The doc claimed no step a producer can forget. A plain word copy still drops a place: psx-toy-story-2
  `hierarchy_drawer.cpp:link` copied the four `rotAverage4` vertices from the scratchpad into the packet with
  `mem_r32`/`mem_w32`, so its new `storeVertex` calls reached nothing. It copies them with `loadVertex`/`storeVertex`
  now. Toy Story 2's mesh drawers still carry bare words (psx-toy-story-2 issue 0056). `presentation.md` states the
  real contract.
- `GteRawState` (`Gte::saveRaw`/`restoreRaw`, so `GteGuard` and `MachineSnapshot`) copied the SXY words without
  their places, and Lightrec's `importGteRegisters` and the save-state GTE section replaced words under old places,
  so `Gte::projected` could pair a guest-written word with a stale place inside the two-pixel window. The raw state
  carries the places, and `placeReplacedScreenEntries` puts every entry whose word an import or a load replaces on
  that word's pixel.
- A render that built its `HostMemory` per call allocated the places vector on every present once vertices were
  stored with places: psx-crash-bash `host_leaf.cpp:emitLeaf`, psx-tomba `list_state_producer.cpp` and psx-toy-story-2
  `hierarchy_draw_state.cpp`. Each now takes a session-kept `Kept<HostMemory>`, which keeps ranges and places across
  presents.
- Tomba's guest-only writers (`perobj_billboard.cpp`, `quad_rtpt_submit.cpp`, `widescreen_margin_quad.cpp`,
  `rain_streaks.cpp:draw`) wrapped guest memory in `EmitMemory` only to call `storeVertex`, which over the guest stores
  the word alone; they store `projected(i).word()` directly.
- Proof: all 11 products build with Clang and run 40 s at real speed, hidden and unpaced, exit 0 (psx-tomba
  `scratch/judder-check/titles/fix`). Tomba 2 boot walk `fx-p3` (load 1.2 to 1.3) with the session-kept memory and
  the (site, list, n-th) key: the new-game intro composes 490 in-betweens with 48 shown whole, as before, and 1
  simulated-display repeat in 1087 presents. The Toy Story 2 hierarchy copy is not shown on screen: no capture tool
  reaches a hierarchy-drawn model's in-between.
