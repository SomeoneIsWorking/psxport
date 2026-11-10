# Replay interpolation: in-betweens from the guest's own draw code

Replaces per-draw-site native producers as the way a title gets 60 fps, real depth and sub-pixel motion.
Native producers stay only where a replay cannot run (a draw section that reaches a device).

## Why

A producer is a byte-exact port of one guest draw routine plus about seven companion facts, and an
in-between is held whole while any moving primitive has none. Spyro 1 keys 99.5% of its primitives and
still shows 30 fps motion; every new area brings new unowned writers. Spyro 2/3's `field_redraw` (one
owner, the guest's own dispatcher re-run at a blended camera and objects) covers what Spyro 1 needed
about 20 producers for.

## Prior art: psxrecomp render passes

`RetroPortingToolKit/psxrecomp` (PolyForm Noncommercial: design only, no code) ships geometry
interpolation in its Tomba 2, Ape Escape and Vigilante 8 ports with `docs/RENDER_PASSES.md`:

- At the pass point (logic of tick n+1 done, before submit), a title plugin replays the game's draw
  section (camera setup through OT build and HUD to the submit) once per planned phase.
- Before each replay, a motion set writes blended values into guest RAM: PsyQ `MATRIX` (rotation
  slerped, translation lerped), translation only, `VECTOR`, `SVECTOR`, 12-bit `ANGLES` (shorter arc),
  `SCALAR`. Values pair by address and identity; a value past a per-title move/turn limit was placed
  (respawn, cut) and keeps its current value.
- The replay runs sandboxed: guest time frozen, no interrupts, device stores dropped, device reads
  abort it, GPU DMA finishes synchronously, VRAM writes outside the display rect journaled, and RAM,
  CPU, GTE, timers and GPU registers restored bit for bit afterwards.
- The replay's image is the in-between, whole. There is no per-object ownership or composition.
- Passes are planned against a measured per-pass cost and shed when the host cannot afford them;
  a frame with no pass image repeats the newest real frame.
- Depth and sub-pixel come from PGXP: a precise shadow of each projected vertex kept with the memory
  word it is stored to.

## Our design

1. **Guest vertex places (PGXP-style).** `VertexDepths` becomes `VertexPlaces`: each word holds the
   full place (sub-pixel x, y, view depth, sort back) of the vertex stored to it, kept while the word
   still holds that value. Lightrec hands every memory callback the opcode, so `LightrecExecutor`
   notes `SWC2` of SXY0..2 with the GTE FIFO place, and carries a place through `LW` into a register
   and `SW` out of it while the register still holds the loaded word. The record tap and
   `PacketChainWalker` read places for guest packets as they do for producer packets. A title names
   its world draw functions and their depth unit (`DepthUnit` scale and `DepthMark`) by guest
   function; the mark travels with the place, so `DepthTest` no longer needs a producer key. Cost: one
   table write per `SWC2`, one lookup per `LW`/`SW` while depth or fps60 is on; nothing scales with ires.
2. **Motion set.** `present/motion_set` holds the kinds above, pairs by address and title identity
   (address plus generation, as `StateProducer` identity does), and applies blends through
   `GuestRedraw::writeWord`. Cuts stay the title's declared cuts; the move/turn limits only catch
   what a cut missed. `blendGteControl`'s slerp is the one rotation blend.
3. **Replay frame.** A title declares its draw section (entry, the arguments captured at the real
   call, and its motion tracks). Inside the logic frame the presenter's planned in-betweens are each a
   `GuestRedraw` of the section; its GP0 stream (the DMA list walk) goes to a device-free record tap
   instead of the device, and that record is the in-between, rastered onto the VRAM before S as a
   composed record is today. A replay that reaches a device, uploads, or does not return refuses that
   in-between only; the present shows S. Per-pass cost (thread CPU time) is measured and passes shed
   above budget. At run end `ReplayFrame::report` logs in-betweens replayed, refused by the section and
   held at assembly, and `MotionSet::report` each track's unpaired record draws (`RunEndReport`).
4. **Titles.** Spyro 1 first (its draw section and motion tracks; the producers it replaces deleted
   once frames show the same picture), then Spyro 2/3's `field_redraw` onto the same owner, then
   Tomba 2.
5. **Delete.** Once no title composes producers, `OwnerLedger`, `OwnershipVerdict`, the per-object
   paths of `FrameComposer` and `EmissionScope` keying go; the ledger's booking on every guest store
   goes with them.

## Not covered by a replay

- Widescreen culls: per-site immediate patches stay title-owned (psxrecomp does the same).
- A vertex the guest computes on the CPU from SXY words (sprite corners, clip crossings) has no
  place and draws on its whole pixel, as under PGXP.
- A draw section that touches a device (a CD read, SPU key-on) cannot replay; that title keeps a
  producer for the section or holds.

## Proof per step

Each step is shown on Spyro 1 at fps60, ires 0, 3840x2160 sink, frames judged as the player sees
them; a framework step also on Tomba 2. Step 1: guest-drawn world polygons test depth and in-between
vertices land off whole native pixels. Step 3: Spyro 1 gameplay in-betweens drawn, halfway between
their neighbours, no hold.

## Evidence: Spyro 1 replay probe (2026-11-10)

A temporary probe in Spyro 1's frame driver ran GamestateDraw's GS_Playing section (CameraUpdateMatrices
through `func_800189F0`, then the bin flatten `func_80016784(0x800)`) in one `GuestRedraw` before each real
draw, with `g_Camera` position and rotation set at t = 0, 0.5 and 1. Stone Hill start and portal, R1/L1 held.

- Returns: 1187 of 1187 gameplay replays returned with no device access or syscall. The pause draw
  `0x8001A40C` reaches a frame boundary: its first frame reads VRAM back for the grey backdrop, so it is
  not a section.
- Identity: the t = 1 replay's packet buffer equals the real draw's byte for byte, links included, in 48
  of 48 dumped frames. Steps must be dispatched with their overrides in force: `GuestRedraw::run`
  suppresses the entry's override and so ran retail's 4:3 particle cull.
- Motion: of moving textured vertices, 96.3% lie within 2 px of halfway (p50 1 px). The outliers are
  sequence-alignment mispairs. The painted t = 0 / 0.5 / 1 buffers show terrain, Spyro, Sparx and
  mobys moving evenly.
- Cost: about 36M host instructions per replay against about 87M for the real draw and present. The
  replay still runs the producers' saves.
- Spyro's section is built and linked inside the draw call; nothing straddles logic frames.

Spyro 2's horn fold under step 1 is not a mixed-site primitive (gating places to single-site primitives
leaves it) and not the depth test (`PSXPORT_DEPTH=0` leaves it); it comes from the sub-pixel positions.

## State (2026-11-10)

Steps 3 and 4 run on Spyro 1 gameplay (`titles/spyro1/core/frame/draw_section.*`, tracks in `draw_motion.*`):
camera, Spyro, his poses and shadow, every moby slot (keyed by props, class and screen space), HUD mobys and
rects, particles, sparkles, the flame and the section's counters. Its gameplay producers are deleted; the
inventory band and shaded line producers stay for their widescreen extension. Stone Hill and Dry Canyon unpaced:
197 of 197 and 184 of 184 moby records pair, gems, Sparx and Spyro land halfway. Not tracked: texture scroll
(Open 6 of the motion-tracks plan) and the Stone Hill ram, whose in-between shows the later pose although its
moby record blends (`psx-spyro` issue 0236). Paced judgement is pending a quiet host. Deleting the producers drops
Spyro 1's keyed depth test on real presents until step 1 returns; step 1 (vertex places) is stashed with the
Spyro 2 horn fold open.

Spyro 2 and 3 (2026-11-10, `psx-spyro` `docs/plans/spyro23-replay.md`): the per-field draw body is the section, every
state of its switch replayed but the loading screen; the moby array is a track with field_redraw's record blend as its
rule, the effects pools a `MotionSource`. field_redraw and the S2/S3 producers are deleted. Glimmer and Sunrise
Spring unpaced pan evenly at every present and pair every moby record; open items in `psx-spyro` issue 0237.

Tomba! 2 (2026-11-10, `psx-tomba` `docs/plans/replay-interpolation.md`): the field draw M:8003F9A8 and the room draw
M:8003FA44 are the section; tracks are the view MATRIX, Tomba's node, the node pools, the 520 render records (their
world MATRIX and part angles, paired by the title's record life) and the effect table. Hazard 3 is closed by
`ReplayFrame::replays` (an S without a section run composes from producers), the depth half of hazard 2 by captured
word depths. Village, Area 1 cave, A04 snow and A08 temple unpaced: Tomba, the pig and the scene land halfway; a
paced village run shows 698 of 702 vblanks at 0.5. No A04 CLUT-upload refusal was seen (hazard 4 open until one is).

Step 5 (2026-11-10) is blocked: producers still compose. Tomba! 2's pause screen and boot (issue 0055 there),
Spyro's shaded line, inventory band, tint and letterbox, every Crash Bash and Toy Story 2 in-between, and the test
program's square producer go through `StateProducer`, `EmissionScope` keying and `FrameComposer`, judged by
`OwnershipVerdict` over the ledger's booking. Crash 1/2/3, CTR, Spider-Man, Vagrant Story, Mega Man X4, Tekken 3 and
C-12 consume none of it. Only the unused parts went: `Composition::Refused` with `HeldCause::ProducerRefused`, and
`inBetweenAt`.
