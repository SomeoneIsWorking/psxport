# Presentation: the one renderer

Every title presents through one renderer: the GPU device executes the guest's GP0 stream, each logic
frame's executed work is a `FrameRecord`, and the record rasterizer draws it on SDL3 GPU. There is no
second draw path. This doc owns what the renderer is, its frame fence and pacing, interpolation,
widescreen and how each part is tested.

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
guest CPU ─ GP1, DMA2/OT walk               gpu_ports, ordering_table, gp0_command
   │
GpuDevice (Beetle gpu.c)                      the only VRAM, GPUSTAT/GPUREAD, display mode
   │  each executed command is appended, in execution order, to
FrameRecord N                                 primitives and VRAM ops; a primitive may carry a key
   │
FramePresenter                                seals N, keeps the last 3 drawing records, owns cadence
   │  every present: FrameComposer(S, producer states S' and S, t)
RecordRasterizer (VK)                         replays N into the VRAM image; draws the composed frame
   │
DisplayPresenter → RenderState          one present per frame or in-between; captures, counters
   │
present plan / sink                           viewport, letterbox, window
```

| module | owns |
|---|---|
| `gpu/gpu_device.*` | Beetle `gpu.c` per `Core`: VRAM, GP0/GP1, GPUSTAT, GPUREAD, display area (incl. PAL) |
| `gpu/gpu_ports.*` | GP1 writes at the device's scanline clock, DMA2 block and linked-list modes; titles call `core.gpuDevice()` for GP0, GPUREAD, `loadImage` and `vram` |
| `present/render_mode.h` | the Core's `RenderPath` (`record` or `device`) and its parse |
| `present/frame_record.*` | ordered primitives and VRAM ops of one logic frame |
| `gpu/gp0_record_tap.*` | executed GP0 command to `FrameRecord` entry, with its key |
| `present/emission_scope.*` | the `(P, obj, k)` scope stack and packet-address to key map |
| `runtime/cpu/native_dispatch.*` | `Producer{Arg}` registration; opens the emission scope around the call |
| `present/frame_state.*` | producer states by the scope serial that saved them; per record, the states its packets came from |
| `present/state_producer.*` | `StateProducer` (render at `t`, and its `Composition` there), `PrimitiveSink` (the primitives a render emits), `PrimitiveList`, the `StateProducers` registry on `Core` |
| `present/guest_redraw.*`, `core/write_rollback.h` | `GuestRedraw`: a guest draw routine run again for an in-between, every store put back, registers and GTE restored, devices and guest time held (`SideEffectMode::Discard`) |
| `present/frame_composer.*` | the frame at `t`: S's unproduced entries plus each producer's render, by OT slot |
| `gpu/ordering_table.*` | OT node decode and the OT walk; `OtTables`, the title-named tables whose bucket heads tag each entry's slot |
| `gpu/record_fault.*` | the render path's refusal of guest data a console never finishes drawing (a draw-list cycle, a packet ending inside a poly-line, a record past `FrameRecord::kMaxEntries` or its pools): one error line naming the guest address, the refusing layer and the guest writer, then abort, before the record is sealed; `LinkCycleCheck`, the cycle check of every linked draw-list walk (OT walk, `PacketChainWalker`, title bin walks) |
| `present/packet_chains.*` | a body's packet chains (one chain, or a bucket table of any stride and head offset) read back as primitives under the one E1 rule; other environment packets and no-ops skipped; `tryEmitChain` reports a packet that is not a primitive instead of aborting |
| `frame/frame_presenter.*` | N and the last three records that drew, which record each present shows, cadence, both presents through the backend |
| `present/display_presenter.*` | `DisplayPresenter` on `Game`: each present and in-between through its `RenderState`, the present counters, `PSXPORT_SHOT_AT`/`PRESENT_SHOT_AT`/`GPU_DUMP` captures |
| `gpu/record_geometry.*` | the VRAM rect an entry writes; a flat rectangle's extent |
| `gpu/record_raster_setup.*` | a record's coverage and interpolants by `gpu.c`'s rules, at scale, per plane, in read-safe batches |
| `gpu/record_raster_regions.h` | written-pixel map and VRAM-wrapped rects the batch split reads |
| `render/record_rasterizer.*` | rasterizes a record in order into the VRAM image and display canvases; `presented()` |
| `render/record_present.cpp` | `RenderPath::Record`: current record to the present source, `shot`, `recordcheck` |
| `render/render_state.h`, `render_present.cpp`, `render_screen.cpp` | `RenderState`, one Game's present: the display picture (record image or device VRAM) or a host picture through the present plan into the present image, then to the sink or a pane |
| `render/render_readback.cpp` | what was presented, read back: `shot`, present shot, VRAM regions |
| `render/render_device.*`, `render_submit.cpp`, `render_window.*` | `RenderDevice`: the SDL3 GPU device, its samplers and pipelines, uploads, downloads, submits and fault latch, the one window and swapchain, the RmlUi library |
| `present/guest_widescreen_projection.*` | the one widescreen API: title aspect and guest projection widening |

Draw order is OT slot order (below). There are no bands, layers or painter ranges.

## Render paths

`PSXPORT_RENDER_PATH` (`config.md`) picks one of two per Core; `Game`'s constructor installs it and logs
`render path: <name>`. The REPL and debug server `renderpath` command switch it live.

- **`record`** (default): presents `FramePresenter::currentRecord()` through `RecordRasterizer`. Every
  title ships on it; widescreen and interpolation exist only here.
- **`device`**: presents the device's own VRAM display rect, 4:3, with no in-betweens. It is the reference
  picture for comparing a record present against.

The device executes every command on both paths and holds the only guest-visible VRAM. Any other
name is refused with a warning and the Core stays on `record`.

## Record path

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
  `record.frag` evaluates texel, modulation, blend and mask per pixel. It does not dither: the host picture keeps full colour precision while the device dithers as the guest asks, so `recordcheck` counts a dithered draw's pixels as mismatched by design. The console shows 5 bits per channel (low three bits zero), so the host picture reads on average 3-4 levels per channel brighter than the console's (psx-spyro 0213: 71/143/56 against 68/139/53; dropping the product's low three bits removes 2.4-3.9). Each op reads a
  snapshot of the image; an op whose reads overlap pixels `gpu.c` wrote at 1x earlier in its batch
  starts a new batch, so at 1x reads see VRAM as the device did. Which ops exist and where batches
  split come from the 1x walk at every S: the `record_plan` line's ops and batches for one frame are
  equal at every S, and only its quads (and the GPU's pixels) grow. The one exception is a triangle whose
  stored places give it area where its words give none, such as a sliver a clip cut between two crossings
  that round to one word: above 1x it draws and writes the native pixels its quads reach (`addTriangle`); without
  it the pieces either side of the sliver leave a seam. A batch's snapshot refresh
  covers every scaled pixel its quads can reach.
- **Scale.** S is the internal-resolution setting (`setires`, `mods.ires`). Vertices and the draw
  area scale; texels and CLUTs stay on the native grid; sprites, lines and fills cover
  S×S blocks; uploads replicate, copies run at full resolution. 24bpp display keeps the device picture.
  `record.frag` finds a scaled pixel's native pixel by integer floor division; a float divide is inexact
  at S = 3 and puts each block's first row and column in the block before.
- **Texture cache.** `gpu.c` fetches texels through a cache that GP0(01), copy, upload, read, soft reset and
  `SetTPage` invalidate (SetTPage runs for E1 and for every textured polygon's own texpage word, and invalidates
  when the page, 4bpp-or-not, or TexDisable changes), and a draw reads pixels it wrote earlier. A textured draw over
  VRAM written since the last invalidation therefore depends on the device's fetch order, which a snapshot read
  cannot reproduce. `TextureFeedback` (`gpu/texture_feedback.*`) follows those invalidation points and tracks the
  VRAM pixels drawn (draw bounds) or filled since; a textured primitive whose sampled texels (its UV box, or the
  whole page axis through a texture window or an 8-bit wrap) overlap one is recorded by the tap as a `VramUpload`
  of the device's pixels over its draw bounds (not interpolated or widened, upload resolution at S > 1). The upload
  keeps the primitive's `RecordKey`; an object with a keyed upload is left as drawn by the composer, since its render
  would draw the baked faces again over their own pixels.
- **Known differences from `gpu.c`.** A draw area below row 511 is clipped at 511 (the device wraps it). Texel
  rows are fetched `& 511`.
- **Interlace.** With 480-line interlace and E1 bit 10 clear the device rasterizes one field per frame: the rows of the
  displayed field are skipped (`skipRowParity`), so the VRAM holds this frame's rows beside the previous frame's. A display
  shows one field at a time, so `RecordRasterizer::selectField` presents the field the shown record drew
  (`FrameRecord::undrawnRowParity`), each undrawn row repeating the drawn row of its pair. The weave stays in the VRAM
  image and in `woven()`, which `recordcheck` compares against the device. A record that draws every row (E1 bit 10 set,
  or no interlace) is presented as drawn. A static scene loses the second field's vertical detail.
- **Capture.** `RecordRasterizer::presented()` is the picture the last present showed (the VRAM
  image's display rect, a canvas, or an in-between copy). `shot`, `preseq` and the fps60 dump all go
  through `record_shot`, which downloads it and applies the present fade; nothing else reads it.
- **Check.** On a title,
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

A producer is registered like any override, with the argument register that names its object
(`Arg::None` when each call draws one object and its arguments are parameters, which must then not
split its identity), and has two halves: the override saves what its render needs, and the render draws from it. The title's
session holds its renders in a `StateProducerTable`, and `GameRuntime::stateProducer` answers from it.

```cpp
dispatcher.install({key(0x8001F798), "drawActor", &drawActor, Producer{Arg::A0}});
session.renders.install(0x8001F798, std::make_unique<ActorRender>());

void drawActor(Core *core) {   // the logic frame: writes the guest's packets, then
  core->frameStates.save(core->emission.current(), ActorState{...});
}
class ActorRender final : public psx::present::StateProducer {
  // At present time, from host memory only: the object at t, into its OT slots.
  void render(const StateInterval &states, PrimitiveSink &) const override;
};
```

The override still writes the guest packets the original writes, so guest-visible memory, the device
and its VRAM are Beetle's. `render` never writes guest memory. It reads the interval's two saved states
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

The serial names a state, not an object, and differs on every scope: `RecordKey ==` compares it, so code that
asks whether two packets belong to the same object compares `RecordKey::identity()`, the key without the serial.

## Interpolation

The composer builds the frame at `t` from S's record:

- **Unproduced entries** (no binding, or bound to a producer without a render) are kept as S drew
  them, at every `t`, only in an interval the verdict found still (below); otherwise the interval is held.
- **Produced entries** (bound to `(P, obj)` of a producer with a render) are removed; `P.render` of
  that object at `t` is emitted instead.
- **Identity.** An object renders interpolated when S' and S both saved state under the same
  `(P, obj)`; saved only in S (born at S), it is left out of the in-betweens and appears at `t = 1`, as it
  was absent in S'; saved only in S', it is not drawn (it is not in the frame). A producer whose skipped
  draw is itself a state (a fade at zero) names it with `StateProducer::rests`: the object saved only in S
  then moves from that state, and one saved only in S' moves toward it, placed by slot. An object S' drew
  with no single state (ambiguous) did not skip its draw, so the interval is held (`Unpaired`). An object S'
  drew without a key is an unowned group of S' that vanished, which the verdict holds. Identity is the producer, the object's guest address and the title's generation for that
  address (Crash Bash's `ComponentIncarnation`), never draw order, position or nearest match.
- **Cut.** A title declares a cut from the guest's own state (camera cut, scene change, teleport);
  across a cut every object renders at `t = 1`. No distance threshold.
- **OT slot.** Every record entry carries the slot it was walked in: the OT table and bucket index of
  the last bucket head the walk passed. A title names its tables (address and length from the guest's
  own OT variables, `core.otTables.name`, with its bucket stride and walk order); a table per display
  buffer shares one id. A title that flattens its buckets into one chain before the walk (Spyro) assigns
  each packet its bucket instead (`OtTables::assign`), good for one walk; a bucket it flattened empty
  is placed after the packet linked before it (`OtTables::placeEmpty`), so the walk still passes its
  head. `render` emits each
  primitive with the slot the guest function would have inserted it into at that state, from the same
  depth-to-bucket rule the original uses. The record keeps where the walk passed each bucket head.
  A producer's primitives go where its entries were in that slot in S (the n-th at the object's n-th
  entry there, so guest draw-mode changes between them stay where the guest put them), or, in a slot it
  did not use in S, at the slot's head, where the last `AddPrim` into it lands; in a bucket S left empty, where the
  table's walk order puts it. A render naming a table S never walked leaves that object as S drew it.
  An in-between (`from` set, `t < 1`) with any object whose render cannot be placed is not composed: the
  interval is held, never a mix of two moments. The `compose` debug channel names the baked, unrendered or
  unpaired object.
- **Composition.** `StateProducer::composition(states)` says what the object's entries become at `t`:
  `Render` (replaced by `render`, the default), `Recorded` (kept as the guest drew them, which is the
  object at `t`) or `Refused` (the object cannot be drawn at `t`; an in-between is then S whole). A
  producer that saves its in-betweens rather than two states answers `Render` only at the `t` it saved
  (`inBetweenAt`, the presenter's points), and refuses a frame whose length it did not save them for.
- **Guest redraw.** A title whose objects are drawn by guest code it does not port can run that code
  again inside the logic frame for the in-between: `GuestRedraw` saves the CPU and GTE, arms
  `Core::writeRollback` so every guest store is noted and put back newest first (such a store binds
  no packet and notifies no code write), and runs the routine with devices unreached, interrupts and
  guest time held. The title writes the in-between's inputs through `writeWord`, reads the packets the
  routine linked with `PacketChainWalker::tryEmitChain` before the redraw ends, and saves them as the
  object's state; a run that does not return, or reaches a device or a syscall, is refused.
- **Environment.** A render sets texture page, texture mode and blend mode; draw area, offset,
  mask, texture window and interlace are those in effect where the primitive lands in S (the
  first recorded primitive at or after its position), and the CLUT from S's sample of the same CLUT
  attribute (`clutWord`), or from the device VRAM when S never sampled it.
- **In-place rewrite.** A title whose producer rewrites an already-written packet (a tint, a palette
  swap, a blend bit) must do it under the packet's own key: `EmissionScope::amend(keyFor(packet))` opens
  the binding the packet already carries, so the rewritten colour word re-binds the command word it
  shares with (`PacketAmendScope`) instead of unbinding it. An out-of-scope rewrite leaves the packet
  unkeyed and its record is shown whole.
- **Line and poly-line packets.** A GP0 line packet decodes to one primitive per segment
  (`decodePolyLine`, first point to last through the terminator), each carrying its place, so a producer
  that keys a node's strip interpolates every line of it.
- **Sub-pixel place.** A screen vertex is a `gte::ProjectedVertex`: the SXY word the GPU reads and its place,
  where it lay before the word rounded it. The GTE hands SXY FIFO entries out only as that type
  (`Gte::projected`, the place from Beetle's projection push); `readData` of SXY0..SXYP aborts, and `writeData` to
  the FIFO places the entry on its word's pixel. `saveRaw`/`restoreRaw` (`GteGuard`, `MachineSnapshot`) carry the
  places; a Lightrec register import or a save-state load puts each entry whose word it replaces on that word's
  pixel (`placeReplacedScreenEntries`), so a direct guest write never pairs with an old place. A body stores a vertex with `EmitMemory::storeVertex` and reads
  one back with `loadVertex`; over a render's `HostMemory` the destination word keeps the place until another
  write covers it, so scratch slots, queued pieces and packets carry it as long as each copy goes through
  `loadVertex`/`storeVertex`. A plain `mem_r32`/`mem_w32` copy, or a word carried through a local, drops the place
  without a report and the vertex draws on its word's pixel; nothing enforces the path. `loadVertex(address,
  word)` reads a vertex whose word the guest re-encoded (an outcode `SXY << 5 | bits`) at the place kept with it.
  Over guest memory only the word is stored: the guest's own arithmetic on SXY words cannot carry places, and the
  GP0 record draws the guest's words. A producer computes a place only for a vertex it makes itself (a clip
  crossing, a split midpoint, a pull, a corner offset by a scaled size, a whole-pixel step with `steppedTo`, a
  lerped 2D point with `nearest`) and stores it through the same path; a local packet is a `gpu::PacketBuffer`.
  `PacketChainWalker` reads each packet's places with its words and `decodePacketPrimitive(PacketView)` gives a
  placed polygon, line or sprite vertex its offset from its whole pixel in `subX`/`subY`; that offset lies within
  two pixels either way, and a place farther off is a word the GPU wrapped, so that vertex keeps its whole pixel.
  No place is matched by value. Every render stores places the same way, so a real (`t = 1`) compose and its
  in-betweens agree and a still object does not shift; a vertex shared by two primitives is one stored word, so
  no seam opens. Above native scale the rasterizer draws a vertex at `whole * scale + floor(offset * scale)`; a
  line's pixels take the offset interpolated along it. At native scale every vertex keeps the guest's pixel. A
  sprite (GP0 0x60-0x7F) takes its origin's place the same way: above native scale its quad and the texel lookup
  move by `floor(offset * scale)` together, so the image slides without resampling (`addSprite`, `record.frag`).
- **Exactness.** At `t = 1`, 4:3, depth testing off, a render must reproduce the entries it replaces;
  the title's oracle compare covers the frame (at 1x; above it, placed vertices keep their offset).

`EmissionScope` binds each packet to `(P, obj)` at store time through the owner ledger and the OT walk stamps the key on its
entries, so the composer knows what a producer replaces. `element` and `part` are not pairing keys;
there are no per-vertex keys. An in-between with nothing produced is the shown record as drawn.

- **Owner ledger.** `OwnerLedger` (`core.owners`) holds one `u32` handle per main-RAM word. A handle names
  a scope instance (`RecordKey` `(P, obj, element, part)`, opened by the dispatcher as `(P, object register,
  0)` or by `instance(object)`) or an unowned writer: the innermost guest function (`GuestWriterLatch`, the
  shadow call stack the block-entry hook keeps), its block-entry PC, the dispatched entry and the image
  generation. Every store through `Core::mem_w*` binds the word to the open scope, or to its writer when none
  is open. A writer storing over a word a scope opened since the last draw list wrote keeps that pair
  (`overwrote`), so an amended owned packet is reported, while pool reuse across frames is not. Port owners
  name draws that reach GP0 without a RAM packet: block DMA, MMIO GP0 and LoadImage. The OT walk stamps each
  node's first command word's handle (header + 4) on its entries. A savestate load clears the ledger.
- **Verdict.** At seal, before any present of S'→S, `OwnershipVerdict` groups S's and S''s unkeyed
  primitives by writer (kind, function or block PC, dispatched entry), OT table and amended flag, and hashes
  each group's picture relative to the draw offset (so a double-buffered still draw matches). A group that
  moved, appeared or vanished makes a finding (`UnownedMoving`, `Port`, `Overwrote`) and holds the interval.
  A keyed packet a writer stored into (`mixedOwner`) is an `Overwrote` finding too and holds the interval:
  its render cannot know what the writer changed. S's groups are kept, so the next interval's S' is not
  collected again.
- **One mode per interval.** The interval is composed whole at every present or held at every present,
  including `t = 1`: the presenter composes every later present first, and a refusal at any of them holds
  all. A held present shows S as the guest drew it. The last present is composed by its own composer
  (`endComposer_`) before the in-betweens and kept for its own present, so a 2-field interval composes twice.

- **Shown record.** A present shows the latest held drawing record with a primitive whose draw area is
  the displayed buffer (`drawAreaSpansBuffer`, the canvas rule): the newest drawing record on a
  single-buffered title, the one before it on a double-buffered one, however many empty records were
  sealed between them. A later record that wrote the displayed buffer (a clear, a fill, an upload, a copy
  elsewhere; a copy onto its own source writes nothing) makes that picture stale: the present is then the
  device's (`FramePresenter::shownRecord`). Records that drew nothing are not held, so their writes are
  kept as rects with their sequence until they are older than the oldest held drawing record
  (`holdUndrawnWrites`). The presenter remembers which record each present showed.
- **Cadence.** With the title's 60 fps on, a logic frame the guest holds for g >= 2 fields gets g presents one
  field apart (`FramePresenter::presentRecords`), so each vblank of a display at the field rate shows the guest a
  field further on; a one-field frame, or any frame with it off, gets one at `t = 1`. Present j shows the guest
  moment a constant latency L before its own field: `t = (j + d + n - L) / n`, where n is the fields between
  S''s and S's draw entries and d those from S's draw entry to this commit, both on the display-field clock
  (`Timing::displayField`, emulated hsyncs over lines per field). L stays as it was while the interval allows
  (`FramePresenter::intervalLatency`): it rises to `d + g - 1` so the last present lands on S, but never so far
  that the first present shows a moment already shown, and falls only when the first present would show a
  moment before S' (`t < 0`). So every composed present shows the guest one field further on, a 2/3 alternation
  lands on thirds and halves, a steady cadence ends on S, and a long gap costs one repeat where L rises and one
  skip where it is forced down, never a run of repeats. A cut or a held interval resets L to `d + g - 1`. A
  composed present whose moment did not advance (the interval had fewer fields than presents) is counted as
  `stalled` in the run-end `records:` line. The `presentseq` debug channel logs each present as it is shown:
  host time, record shown (-1 for the displayed VRAM as it stands, with no record composed), from, `t`, n,
  fields, and the deadline it was scheduled for (`due`). With no record drawing the displayed buffer, S' unknown, or a
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

On the record path `psx::present::latchRecordDisplay` turns the title's aspect into a margin M per side
for the displayed rect. A title's guest projection plan there keeps the retail centre and draw width
(`projectionCenterX`, `guestDrawWidth`): the canvas, not a wider draw area, holds the margins, and the
title widens only what it culls. `RecordRasterizer` keeps a canvas per displayed buffer (at most two, the
least recently shown is dropped; a margin or width change retires both, a height difference between the two buffers does not): the buffer plus M columns each side, at scale, seeded from the image
with black margins and reseeded after a resync. Each record entry is planned once per plane:

- A primitive whose draw area spans the buffer's columns (x range equal, rows inside the buffer; a title may inset its rows) draws into the
  canvas with its x clip widened by M. Any other primitive lands in the canvas only inside the buffer.
- A fill spanning the buffer's columns also fills the margins on its rows. So does a flat untextured rectangle (a sprite, an axis-aligned quad, or a horizontal or vertical line) reaching both edges of the buffer's columns, such as a screen fade (`addCanvasMargins`, psx-spyro issue 0199); the margin continues from the rectangle's own edge. A producer quad placed between rows (an in-between letterbox edge) keeps its rows' fraction, so the margins end on the same scaled row as the centre.
- Copies and uploads land in the buffer region 1:1.
- **Render-to-texture.** Textures, CLUTs and copy sources read the VRAM image, which stays exactly the
  device's. A draw into the display buffer that the guest later samples is read at its 4:3 extent,
  except through a read-back texture.
- **Read-back texture.** A guest that reads a displayed picture back (`StoreImage`), converts it on the
  CPU and loads it as a texture page leaves the margins out. Its title declares the page with
  `GpuDevice::declareReadBack` (`present::ReadBackTexture`: read rect, page, texture mode, index weights)
  while the read rect still holds what was read. `RecordRasterizer` captures those canvas pixels at
  scale, widened by M on a side that meets the buffer edge (`RecordCapture`). From the next record on, a
  textured sprite or polygon mapping the page one texel per pixel samples the capture through the
  weights (`RecordReadBack`, `record.frag` `readBackTexel`) and continues into a margin it reaches. A
  write into the page, a resync or a scale change ends it; at 4:3 nothing is captured; an in-between
  samples but never captures. User: Spyro 1's pause backdrop (psx-spyro issue 0212).

The present shows the canvas at the display's per-column aspect, so 4:3 (M = 0) presents the VRAM
image itself. Known differences: a canvas reaching past column 1023 does not wrap, and a third
displayed buffer evicts one canvas, whose margins are black until redrawn.

### The guest projection latch

A runtime may return a `GuestWidescreenProjection`. Declaring an aspect widens nothing: the title calls
`present::latchGuestProjection` at its measured guest projection publication site and applies the
returned plan to its own projection, culling and clipping. The latch takes three distinct title facts:
the GP1 display extent (decoded by the framework, including the 368-dot mode), the title-authored
projection extent, and the title-authored draw/clip width. `guest_projection_plan` widens each extent
from its own 4:3 width to the smallest even width that does not undershoot the aspect; on `record` the
guest keeps its retail centre and draw width and the canvas holds the margins. The plan is
frame-stable: presentation reads only the stored latch, and invalid or zero title geometry is refused.
`device` is always 4:3.

### What breaks a widening

The horizontal field of view is `OFX / H`, not `H`. Moving OFX outward at unchanged H is a widening;
a smaller H is a zoom. When a wide picture is wrong the projection registers are usually right and one
of these is not:

1. **A horizontal culling owner still comparing against a retail 4:3 literal.** The frustum widens,
   the cull does not, and the margins are empty (Mega Man X4 `is_on_screen` `0x8002B288`, Tekken 3's
   stage-tile wedge `FUN_8006D95C`).
2. **An authored 4:3 2D layout that never uses the margin.** A HUD moves only when a title producer
   anchors it.

`tools/port/widescreen_pair.py` names both verdicts: `WIDENED BUT OFF-CENTRE` and
`WIDENED BUT … MARGIN IS NOT COVERAGE`.

## Frame fence and pacing

`FrameLoopShell::prepareProduct(Game&)` validates the title's `Game::frameDriver`, installs the fatal
VSync trap as the last native binding at that address and requires the measured VSync fact. A
nonnegative `VSync(n)` returns one field's HBlanks (263 NTSC, 314 PAL) in `$v0`. `step` runs one
`stepFrame(Core&, frame)`; the driver calls `Game::presentation.commit(core, guestFields)` exactly once
per frame, `guestFields` being the measured guest display fields it covered (no default). The shell
aborts unless `FramePresenter::fence()` advanced by exactly one.

`GameRuntime::presentDeadline` decides who advances simulated display time and returns the present's host
deadline. The default, `FramePacer::subframeDeadline`, advances Timing, devices and the VBlank latch by the
represented field count. A title with its own field scheduler advances `Timing::advanceDisplayFields` per
delivered field and plans with `FramePacer::presentedFieldsDeadline`, which creates no field. In-betweens
split the deadline of the same fields. Deadlines step at the guest's field rate (`field_rate.h`: NTSC 59.817 Hz, the console's 263 lines of 3413
video clocks), so an NTSC title on a 60.00 Hz display shows one present twice about every 5.5 s; that is accepted. A field that overruns its deadline by more than
an interval drops the debt in whole intervals (`pacePlan`), so later deadlines keep their place on the field grid.
NOPACE suppresses host waiting only. Each `Game` owns its `FramePacer`
and deadline; `paceSubframeFields`, `waitPresentedFields` and `hostScreenPace` plan and then sleep.

Each present, real or in-between, is built, then its show is scheduled for its deadline
(`DisplayPresenter::showAt`). The show happens at the first executor segment boundary past the deadline
(`LightrecExecutor` calls `DisplayPresenter::showIfDue` after every segment), at the next ordering-table node or
GPU DMA block when a DMA2 transfer straddles it (`gpu::submitOrderingTable`, `gpu::dma2Block`: one Spyro DrawOTag
is 2-3 ms of host work inside a single segment), or, waiting for the deadline,
before the next build, show, repaint or pacer plan (`finishShow`), and `FramePresenter::commit` finishes it
before the next frame's composition starts. So the t = 1 present of frame N lands on time while frame N+1's
guest logic runs, and a frame's guest logic, the in-between's composition and build and the t = 1 build share
the whole logic-frame period: no half carries the logic alone (issue 0168). Showing before the wait offsets
the present by that work, and the two presents of a 30 Hz frame then alternate short and long intervals;
building after the wait makes a present late by its build (issue 0161). The line between the two is the swapchain:

- **Build** (`DisplayPresenter::build`, `buildInBetween`, `buildHostPicture` → `RenderState::buildDisplay`
  or `buildImage`): the overlay's CPU frame, the record raster (`presentRecord` →
  `RecordRasterizer::show`, whose plan grows with the internal scale), the device VRAM upload or the host
  picture upload, the letterboxed pass into the present image, and the submit of that command buffer.
  The composed frame (`FramePresenter::composedRecord`) lives only through the build.
- **Show** (`DisplayPresenter::show` → `RenderState::show`): a command buffer that acquires the swapchain
  image without waiting (`acquireSink`), draws the screen into it (`recordScreen`: the present image
  letterboxed, the overlay over it), and submits, which presents. `pshot` draws the same screen into an
  off-screen image at the sink size and reads that back. `GpuPerf::presented` is stamped here. The swapchain is MAILBOX, so
  neither the acquire nor the submit waits for a vblank; a build left in flight on the GPU only delays
  when the shown image is scanned out, not the call.

A pane build with no host attached leaves nothing to show, and `show` does nothing.

**The window** (`RenderWindow`) is one path for players and tools. `PSXPORT_SHOW_WINDOW=1` (the player
launcher, a packaged run) maps it; unset or 0 creates the same window unmapped, claims it with the same
present mode and runs the same build, pace and show, so a tool run stamps `GpuPerf::presented` and its
`[playcheck] presents:` line measures real pacing. On Wayland the tool window is a roleless surface: SDL
counts it as shown, the compositor never maps it, and its swapchain presents (MAILBOX) without waiting. Elsewhere
it is `SDL_WINDOW_HIDDEN`, for which SDL_GPU's acquire returns success with no texture before Vulkan is
asked (`VULKAN_INTERNAL_AcquireSwapchainTexture`, SDL 3.4), so every show is skipped and logged once
(`[sinkidle]`) and the run reports `intervals=0`. The audio device, the default pad recording and
which controllers open follow `RenderDevice::shown()`; presenting never does. A hidden run opens only the
control channel's virtual gamepad and reads the input it injects through the same drain; the hidden
window sets `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, since SDL drops controller input while no window
has keyboard focus. `PSXPORT_PRESENT_SINK=WxH` creates the
window with exactly that drawable in pixels; otherwise it is `kWindowSize` in display units. Check: the log's `[render] window ... roleless, never mapped` line, and KWin's window list
(`qdbus --literal org.kde.KWin /WindowsRunner org.kde.krunner1.Match <title>`) never names it. Unpaced presents (the boot black, a movie's end, a boot card's last frame) build and show at
once. A movie frame is decoded and built before its field wait and shown after it (`Fmv::buildFrame`,
`showFrame`); Tomba's SCEA card builds its host picture, paces, then shows.

A pane present follows the same order through its host (`psx::present::PaneHost`, attached with
`TitleSession::setPaneHost` → `RenderState::setPaneHost`): `buildDisplay` calls the host's `built`
after the pane's build, and `show` calls its `show` after the pane's wait. The picker attaches itself to
the selected, settled panel, builds its own present image at `built` and shows it at `show`, so its cadence
is that panel's presents, in-betweens included. The selector is a host screen of the product: its Game
builds panes into its present image (`DisplayPresenter::buildPanes` → `RenderState::buildPanes`) and
shows it through `showPresentImage`, so the overlay, the RmlUi menu and host input are the ones a title
frame has. The picker's loop builds, paces with `FramePacer::hostScreenPace` and shows itself only on an
iteration where no panel presented, and skips the pace when the panel's step already paced its fields (`title-host.md`). A pane
samples the session's newest present image (`RenderState::lastPresented`; `title-host.md` for when a
panel counts as ready).

A pane's present image is filled, not letterboxed: `PresentInputs::fill` centre-crops the picture to
the image's shape at its displayed 4:3 aspect, at the rendered resolution. The
selector draws each picture 1:1 at its own size, centred on its panel's cover rectangle and cut by the
seams (`PickerPanes`), so nothing is cropped to fit or magnified.

### Auto, a pane, and the internal-resolution budget

Auto is `round(sinkHeight / 240)` — one render sample per PSX scanline — bounded by `videoStatus`'s
budget, which is derived from the whole 1024x512 VRAM at scale and costs real memory because
`RecordRasterizer::ensureImage` holds that image in four textures. The budget is 640 MiB at 14 bytes
per scaled VRAM pixel, which admits 9; `kIresCeiling` caps it at 8.

A pane measures its own present image, not the window. Each panel is given its settled cover rectangle
(`PickerLayout::imageSize`), so the selected panel renders at the display's resolution for its share of
the window; a paused panel keeps the size it last presented at.

The play check's missed-vsync count (`GpuPerf::presented`, the RunEndReport `presents:` line) judges
each present against its own paced deadline. Every pacing call reports the fields it holds the next
present for (`GpuPerf::heldFor`, quota / parts); a movie frame reports the whole fields its predecessor
was shown for (`MovieFieldGrid::show`, below). Reports before one present add up, and a present nothing paced is held for
one field. An interval is a miss when it is over held fields + 0.5 display refresh periods, so a
2-field frame at 33 ms is on time and at 50 ms is late. `longest_miss_run` counts consecutive misses.

## Movies

`Fmv` (`runtime/psx/movie/native_fmv.*`) plays an STR stream on the stream's own clock
(`movie_clock.*`). At `begin` it reads the leading sectors: the sector period is one XA sector's
audio (`xaSectorFormat`) over the XA stride, 150 sectors/s for a silent movie, and the frame slot is
the sector distance between two frame ends that no XA sector separates. Frame n is due when its slot
has been read, so frames are evenly spaced whether or not an XA sector falls in their slot; a stream
whose slot cannot be learned is due at each frame's last chunk. `MovieFieldGrid` shows each frame
from the first display field at or after its due time and reports the whole fields its predecessor
was held for, at most one picture per field.

Audio is the clock the sector period is derived from. The first XA sector is queued when the drive
delivers it, which starts the audio device; later XA sectors are read on their own cursor and queued
one second ahead, so a host stall shorter than that does not starve the device (an underrun would leave
the audio permanently behind the video). The movie ends when its
audio has played. The `fmv` debug line per frame and the `done` line report the audio lag (wall time
minus the device's played position on the movie clock, device latency included); it stays flat when
nothing drifts.

While `Fmv::presenting()`, the movie's frame is the field's only present: a title's frame driver
rotates its fence unpresented (`FramePresenter::commitUnpresented`) and advances its delivered guest
fields itself.

`DisplayPresenter::build` ends each frame's picture: the build, then its dumps. `show` reports the watchdog's
main-present completion once the picture is in the window; a `Transition` present (a movie end, a boot
card) reports watchdog progress only.

## Interpolation availability

`GameRuntime::interpolates()` is the title's answer to whether it ships 60 fps interpolation. `Mods`
reads it before loading settings: a title that does not interpolate refuses a saved or environment
`fps60`, omits `fps60=` on save and has no fps60 row.

## Producer DB

Each title tracks which guest functions draw and which of them have a producer. The row key is the
guest submitter address, the same key the override table uses, so "has a producer" is derived from
the registrations. A run records per row the primitives drawn, how many were keyed, and how many
carried a key another primitive of the same record also carried (ambiguous, so drawn unblended), with
the first such key, and how many keyed primitives had no render (no `StateProducer` at the key's producer, or no state saved for its object, so they show as drawn); the unkeyed rows ranked by primitive count are the producer work queue. Owners: `debug/producer_census.*` (the
per-run table, fed from each sealed record) and `debug/ot_attr.*` (guest packet to submitter). The census is a
diagnostic: it books every guest store, so it runs only with `PSXPORT_PRODUCERS=1` and the player's product never
pays it; nothing a composed frame needs reads it. Armed, the REPL `census` command prints the table so far, and the
run logs its report (the keyed, unkeyed, without-render and duplicated totals, then the ranked rows) at shutdown.

A keyed primitive counts under its key's producer. An unkeyed one counts under the first frame of the
storing call chain that is a registered producer, else the span's emitter; with none it is counted as
no-source, span-miss or span-no-fn.

## Owner journal

`OwnerJournal` (`game.ownerJournal`) turns each interval's findings and refusals into rows, one per reason,
writer or producer and screen: `UnownedMoving`, `Port`, `Baked`, `Overwrote`, `ReplayRefused` and
`IntervalHeld` with its cause (`Cut`, `StaleShown`, `NoPrevious`, `ChainBreak`, `NoState`, `ProducerRefused`,
`Unplaceable`, `Unpaired`). A row carries the guest function, block PC, dispatched entry, image and
generation, the OT span, a sample packet address and GP0 command, intervals seen and lost, primitives, the
screen's title-owned key and warp route (`TitleDebugMenu::screen`) and the `decomp_pipeline.py` command for
the writer. A finding with no writer (a composer refusal, `Baked`) names the declared producer as the function
and resolves its image; a refusal also carries the object's first entry, so its row has an OT span, sample
packet and GP0 command. The screen of an interval is the key `TitleDebugMenu::screenKey` read when S was
sealed, so rows land on the screen S shows. The decomp command (`OwnerJournal::decompCommand`) names the
image through `TitleDebugMenu::decompImage`: the booted executable by default, each overlay's manifest entry
and extracted file where a title declares them (Tomba 2, Toy Story 2); `--target` decompiles the function and
`--function-at` asks the block PC only when it differs. A route may be several control lines separated by
`; `, each run once the screen the one before reaches is shown. Rows the presenter makes for itself
(`StaleShown`, `NoPrevious`, `ChainBreak`, `Cut`) carry no code; they are pacing and history, not RE tasks.
The GTE ordinal of a row's sample is not recorded yet: it needs the per-word place provenance of stage 3. A new row logs one `owners` line; the `owners [n]` control verb lists them, most intervals lost
first; each screen change and run end writes the rows as JSONL under `PSXPORT_OWNERS_DIR`, which a bug
report attaches. The framework test program (`docs/testprog.md`) plants one draw per reason and checks
that the journal lists exactly those.

## Bug report capture

A bug report takes three pictures. `screen.png` is the window's last present. `psx.png` is the guest draw
area from the device VRAM, captured by `BugReportSession::frameSealed` right after `FramePresenter::commit`
seals the next record, so every title produces it through the one commit path; the report says "not
captured" only when no frame seals within 4 fields. `motion.png` is an APNG of the last second of window
presents: `PresentClip` keeps 60 layers of a 640x360 RGBA8 GPU texture array (about 55 MB of VRAM,
allocated on the first window present), letterboxes one GPU blit per swapchain present into the next
layer and records its time, and downloads the ring only when a report is written. Nothing in it is
CPU work per present, and its size does not depend on the internal resolution. The saved setting
`PSXPORT_BUG_REPORT_CLIP=0` turns it off on hardware that cannot spare the memory: no texture, no blit, and
reports carry no `motion.png`.

## Status

The native and GTE draw paths, the render queue, painter, keyed blend and every title's
`RenderCapabilities` are deleted; `record` and `device` are the only paths. Remaining work:
`StateProducer` renders for each title's keyed producers by census rank, the depth attribute and
target, then the opt-in depth test.
