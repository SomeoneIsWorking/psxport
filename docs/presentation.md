# Presentation: match Beetle, then add producers

The target design for every title. `one-renderer.md` and `presentation-contract.md` describe the
mechanisms this replaces; they are deleted once the last title has migrated.

## Order of work

1. **Match Beetle.** The guest runs as it does on Beetle: same devices, same GP0 stream, same frames.
   A real frame is proven by oracle compare (`oracle.md`, `tools/oracle/compare.py` and
   `picture.py`), never by eye or by a counter.
2. **Add native producers** only where a feature needs real geometry: interpolation, widescreen
   margins, depth. A producer is a native override of the guest function that writes the primitives,
   plus a native render of the same geometry from the state it saved.
3. **Verify each producer by oracle compare.** At 4:3 with depth testing off, its render at `t = 1`
   draws what its guest packets drew, so the frame matches Beetle's.

## Structure

```
guest CPU ─ GP0/GP1, DMA2/OT walk            gpu_native_dma, ordering_table, gp0_command
   │
GpuDevice (Beetle gpu.c)                      the only VRAM, GPUSTAT/GPUREAD, display mode
   │  each executed command is appended, in execution order, to
FrameRecord N                                 primitives and VRAM ops; a primitive may carry a key
   │
FramePresenter                                seals N, keeps the last 3 drawing records, owns cadence
   │  every present: FrameComposer(S, producer states S' and S, t)
RecordRasterizer (VK)                         replays N into the VRAM image; draws the composed frame
   │
present plan / sink                           viewport, letterbox, window
```

| module | owns |
|---|---|
| `gpu/gpu_device.*` | Beetle `gpu.c` per `Core`: VRAM, GP0/GP1, GPUSTAT, GPUREAD, display area |
| `present/frame_record.*` | ordered primitives and VRAM ops of one logic frame |
| `gpu/gp0_record_tap.*` | executed GP0 command to `FrameRecord` entry, with its key |
| `present/emission_scope.*` | the `(P, obj, k)` scope stack and packet-address to key map |
| `runtime/cpu/native_dispatch.*` | `Producer{Arg}` registration; opens the emission scope around the call |
| `present/frame_state.*` | producer states by the scope serial that saved them; per record, the states its packets came from |
| `present/state_producer.*` | `StateProducer` (render at `t`), `PrimitiveSink`, the `StateProducers` registry on `Core` |
| `present/frame_composer.*` | the frame at `t`: S's unproduced entries plus each producer's render, by OT slot |
| `gpu/ordering_table.*` | the OT walk; `OtTables`, the title-named tables whose bucket heads tag each entry's slot |
| `present/keyed_blend.*` | the output blend the composer replaces; deleted at migration step 5 |
| `frame/frame_presenter.*` | N and the last three records that drew, which record each present shows, cadence, both presents through the backend |
| `present/in_between_present.*` | the renderer call that shows an in-between without advancing the frame |
| `gpu/record_raster_setup.*` | a record's coverage and interpolants by `gpu.c`'s rules, at scale, per plane, in read-safe batches |
| `gpu/record_raster_regions.h` | written-pixel map and VRAM-wrapped rects the batch split reads |
| `gpu/gpu_vk_record_raster.*` | rasterizes a record in order into the VRAM image and display canvases; `presented()` |
| `gpu/gpu_vk_record_present.cpp` | `RenderPath::Record`: current record to the present source, `shot`, `recordcheck` |
| `gpu/guest_widescreen_projection.*` | the one widescreen API: title aspect and guest projection widening |

Draw order is OT slot order (below). There are no bands, layers or painter ranges.

## Record path

`PSXPORT_RENDER_PATH=record` presents `FramePresenter::currentRecord()` through `RecordRasterizer`;
the device still executes every command and holds the guest-visible VRAM.

- **Tap.** `GpuDevice::gp0` reports each consumed word to `Gp0RecordTap`, which frames packets as
  `gpu.c` ProcessFIFO does and checks after every word that the device agrees (dispatch count, FIFO
  depth, `InCmd`, no dropped word). A disagreement marks the record incomplete until both are idle.
  Draw state, CLUT and sprite flip are read from the device after the command ran.
- **Seal.** `FramePresenter::commit` seals one record per logic frame. It keeps N, which is replayed, and
  the last three records that drew a primitive; a record that drew nothing is no picture. A record
  sealed inside an upload is incomplete; one that starts between a quad's two triangles is too.
- **Raster.** The image is VRAM at scale S (RG8 1555). A record applies only on top of the record
  before it (`sequence == applied + 1`, complete, device not ahead); otherwise the device VRAM is
  uploaded. Sequence 0 is the presenter's empty record; the device's records start at 1. Coverage and interpolant bases are computed on the CPU exactly as `gpu.c` does at S;
  `record.frag` evaluates texel, modulation, dither, blend and mask per pixel. Each op reads a
  snapshot of the image; an op whose reads overlap pixels written earlier in its batch starts a new
  batch, so reads see VRAM as the device did.
- **Scale.** S is the internal-resolution setting (`setires`, `mods.ires`). Vertices and the draw
  area scale; texels, CLUTs and dither stay on the native grid; sprites, lines and fills cover
  S×S blocks; uploads replicate, copies run at full resolution. 24bpp display keeps the device picture.
- **Texture cache.** `gpu.c` fetches texels through a cache that GP0(01), copy, upload, read, soft reset and
  `SetTPage` invalidate (SetTPage runs for E1 and for every textured polygon's own texpage word, and invalidates
  when the page, 4bpp-or-not, or TexDisable changes), and a draw reads pixels it wrote earlier. A textured draw over
  VRAM written since the last invalidation therefore depends on the device's fetch order, which a snapshot read
  cannot reproduce. `TextureFeedback` (`gpu/texture_feedback.*`) follows those invalidation points and tracks the
  64x32 tiles drawn or filled since; a textured primitive whose texture page overlaps one is recorded by the tap as
  a `VramUpload` of the device's pixels over its draw bounds (not interpolated or widened, upload resolution at
  S > 1). The tile is coarser than the cache, so some resolved primitives would have read fresh texels.
- **Known differences from `gpu.c`.** A draw area below row 511 is clipped at 511 (the device wraps it). Texel
  rows are fetched `& 511`.
- **Capture.** `RecordRasterizer::presented()` is the picture the last present showed (the VRAM
  image's display rect, a canvas, or an in-between copy). `shot`, `preseq` and the fps60 dump all go
  through `record_shot`, which downloads it and applies the present fade; nothing else reads it.
- **Test.** `tests/test_record_raster.cpp` replays synthetic GP0 streams through `GpuDevice` and the
  rasterizer at 1x on a headless Vulkan device and requires all of VRAM to match, and checks the
  display canvas against the device drawing the same stream with its draw area widened;
  `tests/test_gp0_record_tap.cpp` covers decode and seal boundaries. `tests/test_record_raster.cpp` also drives
  `FramePresenter` and `RecordRasterizer::show` over a single- and a double-buffered stream with a
  keyed moving triangle and requires each in-between to differ from both reals and to equal the device
  drawing the t = 0.5 triangle; `tests/test_record_cadence.cpp` covers shown-record selection. On a title,
  `PSXPORT_DEBUG=recordcheck` logs per present the display-area pixels that differ from the device
  (at S > 1, each block's top-left pixel), and the replay/resync counts; a composed `t = 1` present at
  4:3 is checked as drawn (`composed=1`), which is a title producer's proof on real frames.

## Frame model

There is no real/in-between distinction. Every present draws the frame at a state `t` between the
record shown before (S', `t = 0`) and the record shown now (S, `t = 1`), through the same code at
every `t`.

```
logic frame N
  guest GP0 packets          -> executed by the device, recorded in N as now
  producer P(obj) runs       -> writes its guest packets (bound to (P, obj), scope serial s), saves state under s
  frame end                  -> FrameRecord N sealed; the states its packets' serials name are collected with it
present at t
  VRAM image                 -> advanced by the guest's own records only (textures stay device-exact)
  picture                    -> FrameComposer(S, State S', State S, t) drawn on the image as before S
```

A producer is registered like any override, with the argument register that names its object, and
has two halves: the override saves what its render needs, and the render draws from it.

```cpp
dispatcher.install({key(0x8001F798), "drawActor", &drawActor, Producer{Arg::A0}});
core.stateProducers.install(0x8001F798, std::make_unique<ActorRender>());

void drawActor(Core *core) {   // the logic frame: writes the guest's packets, then
  core->frameStates.save(core->emission.current(), ActorState{...});
}
class ActorRender final : public psx::present::StateProducer {
  // At present time, from host memory only: the object at t, into its OT slots.
  void render(std::span<const std::byte> from, std::span<const std::byte> to, float t,
              PrimitiveSink &) const override;
};
```

The override still writes the guest packets the original writes, so guest-visible memory, the device
and its VRAM are Beetle's. `render` never writes guest memory. It reads two saved states
(`stateAs<T>`), and guest memory only for data the guest does not change while the object lives (level
geometry); anything that changes per frame is in the state. It owns its own interpolation rules (angle
wrap, which fields step rather than lerp).
A producer that walks a list saves one state per object under `scope.instance(address)`.

Every object scope gets a serial that its packets' keys carry, and a state is saved under the serial
of the scope that saved it. A guest often builds a frame's packets and walks them in the next logic
frame; the record collects the states named by the serials it actually drew (`FrameStates::collect`),
so a picture is always drawn from the state its own packets came from. States are kept for
`kRetainedFrames` logic frames. An object drawn from two scopes, or a scope that saved twice, is
ambiguous and keeps its entries.

## Interpolation

The composer builds the frame at `t` from S's record:

- **Unproduced entries** (no binding, or bound to a producer without a render) are kept as S drew
  them, at every `t`. A missing producer shows up without interpolation, never wrong.
- **Produced entries** (bound to `(P, obj)` of a producer with a render) are removed; `P.render` of
  that object at `t` is emitted instead.
- **Identity.** An object renders interpolated when S' and S both saved state under the same
  `(P, obj)`; saved only in S, it renders at `t = 1`; saved only in S', it is not drawn (it is not in
  the frame). Identity is the producer, the object's guest address and the title's generation for that
  address (Crash Bash's `ComponentIncarnation`), never draw order, position or nearest match.
- **Cut.** A title declares a cut from the guest's own state (camera cut, scene change, teleport);
  across a cut every object renders at `t = 1`. No distance threshold.
- **OT slot.** Every record entry carries the slot it was walked in: the OT table and bucket index of
  the last bucket head the walk passed. A title names its tables (address and length from the guest's
  own OT variables, `core.otTables.name`, with its bucket stride and walk order); a table per display
  buffer shares one id. A title that flattens its buckets into one chain before the walk (Spyro) assigns
  each packet its bucket instead (`OtTables::assign`), good for one walk. `render` emits each
  primitive with the slot the guest function would have inserted it into at that state, from the same
  depth-to-bucket rule the original uses. The record keeps where the walk passed each bucket head.
  A producer's primitives go where its entries were in that slot in S, or, in a slot it did not use in
  S, at the slot's head, where the last `AddPrim` into it lands; in a bucket S left empty, where the
  table's walk order puts it. A render naming a table S never walked, or a CLUT S never sampled, leaves
  that object as S drew it.
- **Environment.** A render sets texture page, texture mode and blend mode; draw area, offset,
  dither, mask, texture window and interlace are those in effect where the primitive lands in S (the
  first recorded primitive at or after its position), and the CLUT from S's sample of the same CLUT
  attribute (`clutWord`).
- **Exactness.** At `t = 1`, 4:3, depth testing off, a render must reproduce the entries it replaces;
  each producer has a unit test on that, and the title's oracle compare covers the frame.

What carries over from the keyed design: `EmissionScope` binds each packet to `(P, obj)` at store
time and the OT walk stamps the key on its entries, so the composer knows what a producer replaces.
`element` and `part` stop being pairing keys; there are no per-vertex keys.

- **Binding.** `RecordKey` is `(P, obj, element, part)`. The dispatcher opens `(P, object register, 0)`;
  `instance(object)` opens another object under the same producer. Every store through `Core::mem_w*` to
  a main-RAM word, native or guest, binds that word to the innermost open scope's key, or unbinds it when
  no scope is open. The OT walk looks up the binding of each node's first command word (header + 4), so a
  reused slot is rebound by its new writer while a link written into the header later leaves the key
  alone. Bindings survive the record seal; a savestate load clears them.

- **Shown record.** A present shows the latest held drawing record with a primitive whose draw area is
  the displayed buffer (`drawAreaSpansBuffer`, the canvas rule): the newest drawing record on a
  single-buffered title, the one before it on a double-buffered one, however many empty records were
  sealed between them. The presenter remembers which record each present showed.
- **Cadence.** With the title's 60 fps on, each logic frame gets two presents, at `t = 0.5` and
  `t = 1`; with it off, one at `t = 1`. With no record drawing the displayed buffer, S' unknown, or a
  record after S' missing or incomplete, the present is `t = 1`.
- **Base state.** The composed frame is drawn onto VRAM as it was before S: the image when S = N, the
  rasterizer's kept pre-state when S is the last applied record with entries. Each plane keeps one
  `before` copy, taken before every replayed record with entries; a record with no entries keeps it, a
  resync drops it. The VRAM image itself only ever advances by guest records, so a later frame that
  samples the display buffer reads the device's pixels, not a composed picture. The `t = 1` present
  draws the composed S, then brings the image to N without showing it; a record with nothing composed
  is shown through the image as before.

## Depth

Depth is a per-vertex record attribute next to the key: view-space depth, later a normal and material.
A producer writes it from its own transform; nothing else fills it yet. The rasterizer writes it to its
own target without changing colour, for host effects (lighting, SSAO, shadows) and for the test below.

Visibility is OT slot order by default, because games are designed around their OT: decals and shadows
win by being drawn later, skies sit in far slots, effects are sorted forward on purpose, and 2D draws
over 3D. A depth buffer applied to all of that is wrong.

- **Opt-in depth test.** A producer may mark primitives depth-tested. Those test and write depth
  against each other only; every other entry draws in slot order with neither. This fixes coarse-sort
  pops inside geometry the producer knows is safe (a terrain mesh), and a producer turns it on only
  once its shots look right. The opt-in is a flag on the emitted primitive, so turning it off restores
  the slot-order picture exactly.
- **Guest depth, later.** An unported guest 3D primitive that sorts visibly wrong against a
  depth-tested producer is the trigger to fill the same attribute from GTE projection tracking (the
  vertex word a projection result was stored to, as PGXP does). It lands in the same attribute and the
  same flag; nothing above changes.

Planned effects, all reading those attributes: dynamic lighting with real shadows, vertex smoothing
(normals averaged across shared vertices) and SSAO. They are chosen by preset (off, low, high), not
per-effect knobs, and every preset leaves the 4:3 oracle picture reachable by turning effects off.
The current SSAO, shadow and light mods are removed, not migrated.

## Widescreen

The guest projection is widened through `GuestWidescreenProjection` (Vagrant Story widens its
canvas, never `H`). Guest 2D is presented as-is, centred; a HUD element moves only when a title
producer anchors it. The margins show whatever the widened projection draws there. 4:3 stays
oracle-exact: every widening is gated on the configured aspect.

On the record path `gpu_vk_latch_record_display` turns the title's aspect into a margin M per side
for the displayed rect. A title's guest projection plan there keeps the retail centre and draw width
(`projectionCenterX`, `guestDrawWidth`): the canvas, not a wider draw area, holds the margins, and the
title widens only what it culls. `RecordRasterizer` keeps a canvas per displayed buffer (at most two, the
least recently shown is dropped): the buffer plus M columns each side, at scale, seeded from the image
with black margins and reseeded after a resync. Each record entry is planned once per plane:

- A primitive whose draw area spans the buffer's columns (x range equal, rows inside the buffer; a title may inset its rows) draws into the
  canvas with its x clip widened by M. Any other primitive lands in the canvas only inside the buffer.
- A fill spanning the buffer's columns also fills the margins on its rows.
- Copies and uploads land in the buffer region 1:1.
- **Render-to-texture.** Textures, CLUTs and copy sources always read the VRAM image, which stays
  exactly the device's. A draw into the display buffer that the guest later samples is read at its
  4:3 extent; the margin is never a texture source.

The present shows the canvas at the display's per-column aspect, so 4:3 (M = 0) presents the VRAM
image itself. Known differences: a canvas reaching past column 1023 does not wrap, and a third
displayed buffer evicts one canvas, whose margins are black until redrawn. The record path does not
use `wide_2d_layout`, `rq_2d_xform` or `ws_2d_local_x`.

## Producer DB

Each title tracks which guest functions draw and which of them have a producer. The row key is the
guest submitter address, the same key the override table uses, so "has a producer" is derived from
the registrations. A run records per row the primitives drawn, how many were keyed, and how many
carried a key another primitive of the same record also carried (ambiguous, so drawn unblended), with
the first such key; the unkeyed rows ranked by primitive count are the producer work queue. Owners: `debug/producer_census.*` (the
per-run table, fed from each sealed record) and `debug/ot_attr.*` (guest packet to submitter). The
REPL `census` command prints the table so far, and every run logs its summary line (keyed, unkeyed and
duplicated totals) at shutdown.

A keyed primitive counts under its key's producer. An unkeyed one counts under the first frame of the
storing call chain that is a registered producer, else the span's emitter, else the render function
of the span's node; with none it is counted as no-source, span-miss or span-no-fn.

## Migration

0. Delete dead code; pictures unchanged.
1. Beetle `gpu.c` becomes `GpuDevice`, per `Core`, the only VRAM. Add `FrameRecord`, the record tap
   and the record rasterizer; replaying a record at 1x 4:3 equals the device's display area. Every
   guest-projection title moves to it; guest packet classification, bands and 2D re-layout go.
2. Producer registration, emission scope, declared cuts (done, with keyed blend as the first composer).
3. Tomba! 2: the record path at 4:3 by oracle compare, its display-time renderer deleted, then its
   producers by census rank, then widescreen margins; `EffectLerp` deleted.
4. The other titles, one at a time.
5. `StateProducer`, `FrameState`, OT slots on record entries and `FrameComposer`, with unit tests on
   synthetic records (identity, birth, death, cut, unproduced hold, slot placement, `t = 1` equals S).
   Spyro 2 terrain first; then each keyed producer gains `save`/`render`, and keyed blend is deleted
   once none is left. Depth attribute and target, then opt-in depth test.
6. Delete the replaced mechanisms: `Fps60` and its strategies, splice, projection provenance, painter
   and depth bands, the fade composite, spyro's `temporal/`.
