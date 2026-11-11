# Presentation: the one renderer

Every title presents through one renderer: the GPU device executes the guest's GP0 stream, each logic
frame's executed work is a `FrameRecord`, and the record rasterizer draws it on SDL3 GPU. There is no
second draw path. This doc owns what the renderer is, its frame fence and pacing, interpolation,
widescreen and how each part is tested.

## Order of work

1. **Match Beetle.** The guest runs as it does on Beetle: same devices, same GP0 stream, same frames.
   A real frame is proven by oracle compare (`oracle.md`, `tools/oracle/compare.py` and
   `picture.py`), never by eye or by a counter.
2. **Replay the title's draw section** for each in-between (Interpolation): the guest's own draw code
   at blended inputs, inside a redraw that puts every store back. A native override stays only where a
   feature needs what no guest word holds (a depth, a widened cull, a place between pixels).

## Structure

```
guest CPU ─ GP1, DMA2/OT walk               gpu_ports, ordering_table, gp0_command
   │
GpuDevice (Beetle gpu.c)                      the only VRAM, GPUSTAT/GPUREAD, display mode
   │  each executed command is appended, in execution order, to
FrameRecord N                                 primitives and VRAM ops; a primitive may carry a key
   │
FramePresenter                                seals N, keeps the last 3 drawing records, owns cadence
   │  every in-between: S with the title's draw section replayed at t (ReplayFrame)
RecordRasterizer (VK)                         replays N into the VRAM image; draws the replayed frame
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
| `present/guest_redraw.*`, `core/write_rollback.h` | `GuestRedraw`: a guest draw routine run again for an in-between, every store put back, registers and GTE restored, devices and guest time held (`SideEffectMode::Discard`) |
| `gpu/ordering_table.*` | OT node decode and the OT walk; `OtTables`, the title-named tables whose bucket heads tag each entry's slot |
| `gpu/draw_list_cycle.*` | the refusal of a draw list that revisits a node, which a console's walk never leaves: the walk stops at the revisit and latches the presentation device off (`FaultCause::DrawListCycle`, one error line naming the node, the refusing walk and the writer of the edge into it), so nothing of that frame reaches the GPU and the session ends with the player told; `LinkCycleCheck`, the cycle check of every linked draw-list walk (OT walk, `PacketChainWalker`, title bin walks). A record past `FrameRecord::kMaxEntries` or its pools is port storage, not a console limit: it warns and is marked incomplete |
| `present/packet_chains.*` | the packets of a chain a replayed section linked, captured with their words' places |
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

The device executes every command on both paths and holds the only guest-visible VRAM; its software raster
is the largest CPU cost the port keeps, since the guest reads that VRAM. Any other
name is refused with a warning and the Core stays on `record`.

## Record path

- **Tap.** `GpuDevice::gp0` reports each consumed word to `Gp0RecordTap`, which frames packets as
  `gpu.c` ProcessFIFO does and checks after every word that the device agrees (dispatch count, FIFO
  depth, `InCmd`, no dropped word). A disagreement marks the record incomplete until both are idle.
  Draw state and sprite flip come from the tap's own `Gp0DrawState` (E1-E6 and a textured polygon's texpage
  decoded from the words; GP1(09) and the interlaced field are taken at GP1 and status steps), and every word
  checks it against the device's registers: a difference is a named fault. The CLUT attribute comes from the
  packet words and is checked against the device's CLUT cache tag; the CLUT entries are still copied from the
  device's cache. After a desync or a state load the tap reseeds from the device.
- **Chain decode.** `gpu::decodePackets` decodes captured chain packets (`PacketChainWalker::capture`) with the
  same `Gp0DrawState`, so a chain that never reached the device yields the entries the tap would have recorded,
  with the CLUT left unresolved (`clutWord` only). Seeded from a `RecordDrawState`, it starts with E1's flip,
  texture-disable and dfe bits clear.
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
  it the pieces either side of the sliver leave a seam. A tested sliver covers its neighbours' native pixels, so it
  also starts a depth group. Spyro 1 Stone Hill at 8x has about 48 slivers and 29 extra groups per record, +3% ops
  and +37% draws over 1x (psxport issue 0199). A batch's snapshot refresh
  covers every scaled pixel its quads can reach. The 1x walk is CPU work every frame at any S; a line plans as one quad
  per pixel.
- **Scale.** S is the internal-resolution setting (`setires`, `mods.ires`). Vertices and the draw
  area scale; texels and CLUTs stay on the native grid; sprites, lines and fills cover
  S×S blocks; uploads replicate, copies run at full resolution. A texel reads the pixel of its S×S block
  where the fragment's UV falls inside it (a triangle's interpolant fraction, a sprite's pixel within its
  native pixel; `record.frag` `texel`): an uploaded block is uniform and reads as at 1x, while a block a
  draw or a placed copy wrote at scale shows its detail. 24bpp display keeps the device picture.
  `record.frag` finds a scaled pixel's native pixel by integer floor division; a float divide is inexact
  at S = 3 and puts each block's first row and column in the block before.
- **Texture cache.** `gpu.c` fetches texels through a cache that GP0(01), copy, upload, read, soft reset and
  `SetTPage` invalidate (SetTPage runs for E1 and for every textured polygon's own texpage word, and invalidates
  when the page, 4bpp-or-not, or TexDisable changes), and a draw reads pixels it wrote earlier. A textured draw over
  VRAM written since the last invalidation therefore depends on the device's fetch order, which a snapshot read
  cannot reproduce. `TextureFeedback` (`gpu/texture_feedback.*`) follows those invalidation points and tracks the
  VRAM pixels drawn (draw bounds) or filled since; a textured primitive whose sampled texels (its UV box, or the
  whole page axis through a texture window or an 8-bit wrap) overlap one is recorded by the tap as a `VramUpload`
  of the device's pixels over its draw bounds (not interpolated or widened, upload resolution at S > 1). A replay
  whose run bakes an upload gives no in-between.
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
  (at S > 1, each block's top-left pixel), and the replay/resync counts.

## Frame model

Every present draws the record shown now (S, `t = 1`) or, at 60 fps, an in-between at `t` from the record
shown before (S', `t = 0`): S with the title's draw section replayed at `t` (Interpolation), or S as the guest
drew it. The VRAM image advances by the guest's own records only, so textures stay device-exact.

A native override registered with `Producer{Arg}` opens an object scope around its call, the argument register
naming its object (`Arg::None` when each call draws one object). Every object scope gets a serial that its
packets' keys carry, so the verdict and the owner journal know which packets a declared owner wrote.

The serial names a state, not an object, and differs on every scope: `RecordKey ==` compares it, so code that
asks whether two packets belong to the same object compares `RecordKey::identity()`, the key without the serial.

## Interpolation

**Replay.** A title whose `GameRuntime::replayFrame` returns a `ReplayFrame` draws its in-betweens with its own
draw code. Before each real run of the draw section the frame
driver calls `beforeDraw`: the `MotionSet` samples the title's tracks (guest words with a `Motion` each, or a
`MotionRule` for words that move as a unit, such as a keyframe pose). A track is one record or a run of them
(`MotionRecords`: a stride, an optional pointer to the first, an end byte, and a live byte or, for a pool whose
records carry no such byte, the title's `MotionLiveness`); two samples of a record pair at the same address only
while both are live, its `MotionKey` words hold and, where the title's allocator rather than the record's bytes
names a new object, its `MotionIncarnation` gives both the same life; an unpaired record is replayed as drawn. A PsyQ MATRIX moves by
`TransformRule` (rotation along the shorter arc, translation straight; a view matrix, `Translation::View`, moves its camera -RᵀT straight instead, so an object the camera follows holds its place on screen while the view turns: `gte_control.h:viewTranslationBetween`); a scroll by `RingRule` (each halfword the short way round its period, fixed or held in the record's next word); a 12-bit angle moves the short way round and
carries into the whole turns above it (`blend.h:turnedBetween`, rounded by `lerpTurned`; a title keeping a fraction below the
unit reads `turnedBetween`). Inputs that pair by the title's own rule (pools whose entries name lists of their own) are a
`MotionSource`, sampled and written with the tracks; its write returns what it drew and how much of that was
unpaired, reported at run end beside the tracks. The title calls `forget` at a cut it sees before the draw so the next sample pairs with nothing (a state restore forgets for it, `MachineState::restore`); a cut it declares after the draw holds the interval in the presenter. Once two samples pair the section is
re-run in a `GuestRedraw` for each k/n of the interval with the tracks written k/n of the way. While it runs,
`ReplayFrame::replaying` gives k/n, so a title override that reads or writes what no guest word can hold at a
fraction (a result each caller reads at its own index, an edge one row per draw) blends it there. A blended word is
whole; an override that draws from a tracked word reads its two samples (`MotionSet::pairedRecord`, through
`ReplayFrame::motion`) and the rule's unrounded value (`RingRule::exactHalf`) and places its vertices by the
difference (`EmitMemory::storeVertex`, or `GuestRedraw::writeVertex` from a motion source's write), so a one-unit step lands halfway rather than on one end. A motion source moves a copy packet the same way (`GuestRedraw::writeCopy`): its source, destination and size words each keep the part past their whole pixels, `decodeCopy` reads them into the copy's `CopyPlace`, and `addCopy` draws its rect at scale (`scaledCoordinate`, so two copies sharing an edge meet on one scaled row) and shifts its source read by the places' difference (`kOpShiftX/Y`); one that wraps VRAM or overlaps its source is drawn at its whole pixels. The chain head the
section returns is captured (`PacketChainWalker::capture`). At seal the captures belong to S; in-between k is S
with the one chain run the section names (`ReplaySection::linked`) replaced by the capture decoded under S's draw
state at that run (`FrameRecord::splice`, `gpu::decodePackets`), CLUTs resolved by `RecordCluts`: one S sampled
keeps its entries; one S never sampled is read from VRAM. A palette cycle is the title's to declare
(`PaletteCycles::declare`, sampled with the set into `CycledRows`): each 16-entry row its owner uploads and the guest
table of frames it uploads from. A declared row S' and S both sampled (same attribute, 4-bit) as two different frames
of its table is blended k/n of the way from S''s frame to S's (each 5-bit channel by `lerpInt`, the STP bit S's, the
transparent 0x0000 never blended into, out of or landed on); any other CLUT change between ticks lands on S. A
replayed in-between keeps S's entries. Each captured word keeps the depth a draw site noted with it (`EmitMemory::capturedPlace`), so replayed primitives test
depth as the real present's do. A run that bakes an upload, a capture that does not decode, or no single matching
run gives no in-between, and an interval with no replayed set is held (`not-replayed`). An S whose draw never ran
the section (`ReplayFrame::replays`: a menu, a pause, a load) shows S as the guest drew it at every in-between. Spyro 2 and 3 replay their whole per-field draw body, state switch included; Tomba! 2 its field and room
draws. Check: Spyro 1 Stone Hill walking, `presentseq` advances
0.5 logic frames per simulated 60.00 Hz vblank, Spyro, Sparx and the gems move halfway between real presents, and
the `motion` debug channel counts each track's paired records.

- **Cut.** A title declares a cut from the guest's own state (camera cut, scene change, teleport);
  across a cut S is shown as drawn. No distance threshold.
- **OT slot.** Every record entry carries the slot it was walked in: the OT table and bucket index of
  the last bucket head the walk passed. A title names its tables (address and length from the guest's
  own OT variables, `core.otTables.name`, with its bucket stride and walk order); a table per display
  buffer shares one id. A title that flattens its buckets into one chain before the walk (Spyro) assigns
  each packet its bucket instead (`OtTables::assign`), good for one walk; a bucket it flattened empty
  is placed after the packet linked before it (`OtTables::placeEmpty`), so the walk still passes its
  head. The record keeps where the walk passed each bucket head.
- **Guest redraw.** A title whose objects are drawn by guest code it does not port can run that code
  again inside the logic frame for the in-between: `GuestRedraw` saves the CPU and GTE, arms
  `Core::writeRollback` so every guest store is noted and put back newest first (such a store binds
  no packet and notifies no code write), and runs the routine with devices unreached, interrupts and
  guest time held. The title writes the in-between's inputs through `writeWord` and the
  packets the routine linked are captured before the redraw ends (`PacketChainWalker::capture`); a run that does not return, or reaches a device, a syscall or a host service whose state lives
  outside guest memory (every BIOS call but the libc leaves, the pad work area), is refused: the rollback cannot put
  that state back, and a replayed `TestEvent` would take the card event the real frame waits for. A host body that
  reaches one stops there (`DiscardedRunTouchedDevice`), so a native loop polling a device status cannot spin.
  A redraw names the host services whose work is the real frame's (`GuestRedraw::applied`, a sound a draw starts):
  the Discard journal withholds each wherever the run reaches it (`SideEffectJournal::withhold`) and the call
  returns at its ra with the registers as it found them, so only call sites that read nothing back may name one.
- **Transform.** A track that moves a GTE transform by t blends it with `blendGteControl` (`gte_control.*`): the
  rotation turns along the shorter arc (`lucent::rotation::between`: polar decomposition, slerp) and its stretch moves element by element, so a
  spinning part keeps its size between frames; translation moves by t, rounded to the nearest integer with ties to
  even (`blend.h:lerpInt`), so an odd step at t = 0.5 favours neither frame. A singular matrix, or a mirror paired with a
  rotation, has no turn between them and blends element by element.
- **In-place rewrite.** A title whose override rewrites an already-written packet (a tint, a palette
  swap, a blend bit) must do it under the packet's own key: `EmissionScope::amend(keyFor(packet))` opens
  the binding the packet already carries, so the rewritten colour word re-binds the command word it
  shares with (`PacketAmendScope`) instead of unbinding it. An out-of-scope rewrite leaves the packet
  unkeyed and its record is shown whole.
- **Line and poly-line packets.** A GP0 line packet decodes to one primitive per segment
  (`decodePolyLine`, first point to last through the terminator), each carrying its place, so a replayed
  strip places every line of it.
- **Sub-pixel place.** A screen vertex is a `gte::ProjectedVertex`: the SXY word the GPU reads and its place,
  where it lay before the word rounded it. The GTE hands SXY FIFO entries out only as that type
  (`Gte::projected`, the place from Beetle's projection push); `readData` of SXY0..SXYP aborts, and `writeData` to
  the FIFO places the entry on its word's pixel. `saveRaw`/`restoreRaw` (`GteGuard`, `MachineSnapshot`) carry the
  places; a Lightrec register import or a save-state load puts each entry whose word it replaces on that word's
  pixel (`placeReplacedScreenEntries`), so a direct guest write never pairs with an old place. A body stores a vertex with `EmitMemory::storeVertex` and reads
  one back with `loadVertex`; over a `HostMemory` the destination word keeps the place until another
  write covers it, so scratch slots, queued pieces and packets carry it as long as each copy goes through
  `loadVertex`/`storeVertex`. A plain `mem_r32`/`mem_w32` copy, or a word carried through a local, drops the place
  without a report and the vertex draws on its word's pixel; nothing enforces the path. `loadVertex(address,
  word)` reads a vertex whose word the guest re-encoded (an outcode `SXY << 5 | bits`) at the place kept with it.
  Over guest memory only the word is stored: the guest's own arithmetic on SXY words cannot carry places, and the
  GP0 record draws the guest's words. An override computes a place only for a vertex it makes itself (a clip
  crossing, a split midpoint, a pull, a corner offset by a scaled size, a whole-pixel step with `steppedTo`, a
  lerped 2D point with `nearest`) and stores it through the same path; a local packet is a `gpu::PacketBuffer`.
  `PacketChainWalker` reads each packet's places with its words and `decodePacketPrimitive(PacketView)` gives a
  placed polygon, line or sprite vertex its offset from its whole pixel in `subX`/`subY`; that offset lies within
  two pixels either way, and a place farther off is a word the GPU wrapped, so that vertex keeps its whole pixel. The
  record tap (`Gp0RecordTap`) gives a real present's polygons, lines, poly-line points and sprites their places
  from the same words through the same decode, so a replay at `t = 1` draws the real present exactly.
  No place is matched by value. Every replay stores places the same way, so a real present and its
  in-betweens agree and a still object does not shift; a vertex shared by two primitives is one stored word, so
  no seam opens. Above native scale the rasterizer draws a vertex at `whole * scale + floor(offset * scale)`; a
  line's pixels take the offset interpolated along it. At native scale every vertex keeps the guest's pixel. A
  sprite (GP0 0x60-0x7F) takes its origin's place the same way: above native scale its quad and the texel lookup
  move by `floor(offset * scale)` together, so the image slides without resampling (`addSprite`, `record.frag`).
`EmissionScope` binds each packet to `(P, obj)` at store time through the owner ledger and the OT walk stamps the key on its
entries, so the verdict knows which packets a declared owner wrote. `element` and `part` are not pairing keys;
there are no per-vertex keys.

- **Owner ledger.** `OwnerLedger` (`core.owners`) holds one `u32` handle per main-RAM word. A handle names
  a scope instance (`RecordKey` `(P, obj, element, part)`, opened by the dispatcher as `(P, object register,
  0)` or by `instance(object)`) or an unowned writer: the innermost guest function (`GuestWriterLatch`, the
  shadow call stack the block-entry hook keeps), its block-entry PC, the dispatched entry and the image
  generation. Every store through `Core::mem_w*` binds the word to the open scope, or to its writer when none
  is open. A writer storing over a word a scope opened since the last draw list wrote keeps that pair
  (`overwrote`), so an amended owned packet is reported, while pool reuse across frames is not. Port owners
  name draws that reach GP0 without a RAM packet: block DMA, MMIO GP0 and LoadImage. The OT walk stamps each
  node's first command word's handle (header + 4) on its entries. A savestate load clears the ledger. Booking is
  paid on every guest store.
- **Verdict.** At seal, before any present of S'→S, `OwnershipVerdict` groups S's and S''s unkeyed
  primitives by writer (kind, function or block PC, dispatched entry), OT table and amended flag, and hashes
  each group's picture relative to the draw offset (so a double-buffered still draw matches). A group that
  moved, appeared or vanished makes a finding (`UnownedMoving`, `Port`, `Overwrote`) and holds the interval.
  A keyed packet a writer stored into (`mixedOwner`) is an `Overwrote` finding too and holds the interval:
  its owner did not draw what the writer changed. Only a word the packet's own scope wrote counts: a word
  another scope left past its cursor (a drawer's store at a primitive it then rejected) is free pool memory
  the next drawer's packet reuses, not an amendment. S's groups are kept, so the next interval's S' is not
  collected again.
- **One mode per interval.** The interval is replayed at every in-between or held at every present: the
  presenter replays the last present first, and a refusal at any of them holds all. A held present shows S
  as the guest drew it: a held in-between builds N's own present (no replayed record), so the record is planned and rastered once per interval and the `t = 1` present finds it applied.
  Where S is not N (a double-buffered title shows N-1 while N draws the other buffer), the displayed buffer's
  rect of N's image is still S's picture: S is the latest record that draws that buffer, and an undrawn write
  over it after S leaves no shown record.

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

Visibility is OT slot order by default, because games are designed around their OT: decals and shadows
win by being drawn later, skies sit in far slots, effects are sorted forward on purpose, and 2D draws
over 3D. A depth buffer applied to all of that is wrong. Native depth corrects only the coarse sort
between opaque world meshes, where a polygon's bucket says less than its pixels' real distance (a ledge
trim over Spyro's feet, a wall seam over the floor), and leaves everything else to the slot order.

- **Where depth comes from.** The Beetle GTE hook keeps each projection's view depth beside its
  sub-pixel place (`GtePlace.z`: Beetle's SZ3, at least H/2). `Gte::projected` maps it through the open
  `psx::cpu::DepthUnit`: SZ times `scale`, less `nearer`, in the title's common view units, 0 (unknown)
  when that is not in front of the eye. `scale` is derived from the draw site's own transform against the
  common one (Spyro 1: terrain views the camera word >> 2; a moby's translation is that view << 2 >>
  its position shift, times scale / 32; Spyro's centre is world minus camera, so 1/4; the far terrain
  pass works in world >> 4, so 4; a terrain grid point near the eye projects at 16x and the guest shifts its
  SZ back, so that projection names 1/16). `nearer` is the distance the guest itself sorts the draw nearer
  than it lies, read from the guest's own bin bias in bins of 32 view units: a moby's z step and Spyro's
  depth-bias byte are nearer, the far terrain pass's base (Spyro 1 0x40 bins) is farther, so the
  low-detail mesh never covers near terrain, as its slot never does. A guest draw whose translation a
  native site wrote takes its unit from that word: the site notes the TRZ word with a `NotedPlace.translation`
  (scale, nearer), an `LW` carries it in the GPR, and a guest `CTC2` of TRZ hands it to the GTE
  (`Gte::carryTranslation`, fed by `LightrecExecutor::cop2Transfer`), whose projections multiply it into the open
  unit until the next TRZ write; an untagged word or a host write carries nothing. Spyro 2/3's moby walk tags each
  render-list entry's centre z this way for the guest moby drawer, with the `slot` of its per-face buckets, which
  each projection's `back` gains, and the entry's address as its `model`, which each projection's place carries
  (`GtePlace.model`) into the packet's vertices. A producer's computed vertex (a split midpoint, a resplit) carries `screenBlendDepth`
  of its sources: 1/z is linear on screen.
- **Sort-back.** A draw site may also say how far back the guest sorts a polygon beyond its depth
  (`GtePlace.back`, view units). Spyro's terrain bins a polygon by its depth sum plus the draw word's bias
  (`guest_terrain_polygon.h:polygonBin`); its producers set each vertex's `back` to that bias
  (`sortedBack`, bins of 32 view units). Only the depth the polygon leaves for later draws lies back by it;
  the polygon itself tests at its true depth (How it draws).
- **Face sort.** A guest drawer that buckets each face by its summed SZ less a bias in its face word
  (`psx::cpu::FaceSort`, `VertexPlaces::sortFaces`) names its function, the `LW` opcode that reads each face
  word, its bin and the face word's bias in slots. That load opens the face (`VertexPlaces::faceLoaded`, from
  `LightrecExecutor`'s load callback behind a one-bit opcode prefilter); each vertex word the drawer then loads
  back from where it projected it and stores into the packet is noted that many slots nearer (a slot: `bin` plus
  the vertex's `back`) and its `back` that much farther, so the face tests where the guest sorts it and leaves
  its own depth. Spyro 2/3's moby drawer, `lw $s0, 4($fp)`.
- **Who keeps it.** Over host memory the place travels with the word (`HostMemory`). Over guest
  memory `psx::cpu::VertexPlaces` (`Core::vertexPlaces`, PGXP's memory shadow: one flat table over main RAM and
  the scratchpad, allocated by the first note) keeps each vertex word's place (sub-pixel x and y, view depth,
  sort-back) and the function that placed it, while the address still holds that word; a bit per word says which are placed, so
  a load or store of any other word tests one bit. A guest `SWC2` of an SXY
  entry notes the GTE's place under the enclosing guest function (`GuestWriterLatch`, which also holds a function
  the host enters without a `jal`: `dispatchGuest`, `callOriginal` and the resumes push it for the call's extent); `MFC2` of an SXY entry and
  `LW` of a placed word carry the place in the GPR, and `SW` of that GPR while it still holds the word notes it at
  the new address (`LightrecExecutor` feeds all four from Lightrec's opcode-carrying callbacks). A native
  override's `EmitMemory::storeVertex` notes its place under the override's entry (`Core::active_native_address`).
  Every guest store (`Core::bookStore`) forgets the words it touches, so a later writer of the same word, a reused
  packet buffer included, never inherits a stale place; a replay's stores forget too. Every change to the table
  inside a replay (notes, forgets, carried registers) goes back with its stores (`WriteRollback`), and
  `GuestRedraw` puts the writer latch back, so a replay leaves places and attribution as it found them. The table tracks while presents draw places (ires above 1 or the depth setting on,
  `RenderState::presentRecord`); off it is released. The OT walk passes each command word's place in
  `Gp0Origin.place`, and the tap gives each polygon vertex its place (`applyPlace`: depth, `back`, and the
  sub-pixel offset when within two pixels of its word) and the primitive the `site` that placed every vertex. A
  replay's capture reads the same places through `EmitMemory::readPlaces` and each packet's site through
  `EmitMemory::placingSite`, so in-betweens carry sub-pixel places, depth and site as real frames do. A vertex the
  guest computes on the CPU from SXY words has no place and draws on its whole pixel, as under PGXP.
- **Which primitives test.** `Core::depthTest` (`psx::present::DepthTest`) holds the draw functions a title
  marks by guest entry (a guest function, or a native override's entry), each with its `DepthMark` (order and bin):
  opaque world 3D (terrain, mobys, the player model), and a 2D fill placed at the depth of the world plane it
  covers (a portal opening: the plane through its projected outline, psx-spyro issue 0224); never HUD, sky,
  translucent, decal or shadow draws. A primitive tests when it is a polygon, opaque, its `site` is a marked
  function and every vertex has depth > 0; a draw site that projects with no view depth (a HUD model, `DepthUnit`
  scale 0) leaves its vertices at 0 and draws in slot order. `Polygon` (a mesh the guest sorts per polygon: Spyro 1
  terrain, Spyro 2/3's terrain and player drawers, the latter bucketing each face by its summed SZ) tests and writes
  per polygon, with the mark's `bin`: the depth one slot of the guest's sort spans (Spyro terrain: 32 view units; the
  Spyro 2/3 player buckets: 4). `Object` (a model the guest sorts into one slot, its parts painter-ordered: a horn base over a
  crest) follows the guest's own rule for a model: nothing the slot order drew before it covers it, so it draws
  over that scene in its own order without testing, and writes its depth, so what the slot order puts after it (a
  ledge trim in a nearer bin) is tested against it. `Model` (a model whose parts the guest paints in its own
  bucket order, Spyro 2/3's moby drawer, named per polygon by its vertices' one `model`) tests each part against
  everything drawn before it except its own model's parts, which it paints over in order: its own order stands,
  nearer scenery and other models drawn before it still hide it, whatever lies between its parts in the record.
  Each part leaves the nearer of its depth moved back by `back` and the depth already there, so a part that lost
  to nearer terrain leaves that terrain's depth. A `Model` polygon whose vertices name no model or two draws in
  slot order.
- **Contact within the sort's resolution.** Surfaces that touch within one bin are ordered by the slot
  order, as retail orders them: a later `Polygon` loses only where its true depth lies farther than the
  depth there, and the depth a `Polygon` leaves is its own moved back by its `bin` and its `back`. So the
  Peace Keepers fairy pad's buried skirt stays under the sand drawn after it, and the Toasty start plinth's
  skirt, sorted after the grass by the grass's bias, stays over it (psx-spyro issue 0233). An `Object`
  leaves its true depth: a terrain polygon after it beside Spyro's toes still loses to them. Not expressed: an
  unmarked or `Object` draw over nearer terrain that the slot order then hides with a later, farther polygon
  stays visible, because that polygon loses to the nearer terrain's depth (a Stone Hill tree's trunk base over a
  grass hill, psx-spyro issue 0233).
- **How it draws.** A tested triangle is followed in the op buffer by a `DepthPlane` op (never drawn)
  holding its 1/z plane over native coordinates; the op carries `kOpDepthTest`, and an `Object`
  polygon also `kOpDepthObject`; the plane's `kDepthBin` holds the polygon's bin plus its `back`. Only the
  `RECORD_DEPTH` variants of `record.frag` read the plane and write `log2(z) / 16` (`RECORD_DEPTH_BACK`, the draw
  that marks, moves z back by `kDepthBin`) to a D24S8 target (D32S8 where the GPU lacks it). A
  plane's depth and stencil clear on its first pass of each record. The test and the depth it leaves
  differ, which one fixed-function test cannot do, so the planner splits tested `Polygon` ops into depth
  groups (`RecordPlanner::Builder::depthGroup`): a group's ops cover no native pixel twice, and the next op
  that would starts a new group (parity bit `kOpDepthGroup`). A group draws twice in its pass: LESS_OR_EQUAL
  with no depth write, its colour where it passes and stencil 1 there; then ALWAYS with depth write and no
  colour where stencil is 1, writing the moved-back depth and clearing the stencil. The stencil's high bit is
  that pass mark; its low seven bits name the `Model` whose depth a pixel holds, 0 for any other. A `Model` op
  (`kOpDepthModel`) carries its model as a stencil id in its plane's `kDepthModel` (1..127 by first appearance in
  the record, `RecordPlanner::Builder::modelId`; the 128th model shares the first's id, which makes their shared
  pixels paint in slot order). A run of one model's ops draws four times: LESS_OR_EQUAL where the id is not its
  own and ALWAYS where it is, both with colour, no depth write and the pass mark; then where marked, no colour,
  LESS_OR_EQUAL with depth write of the moved-back depth, naming its id where the write lands; then clearing the
  mark. A tested `Polygon` that passes and an `Object` clear the id. An `Object` run draws
  ALWAYS with writes, so its last-drawn part's depth is the one kept, as its colour is; every other run uses
  the plain shader with ALWAYS and no write, in the same OT order, so marked and unmarked draws interleave
  exactly as the slot order says. A pass with none uses the pipeline with no depth target. Overlap is
  judged on native pixels, as batches are; where two group members share only a scaled pixel, the later one
  keeps it, as slot order does.
- **Interpolation.** In-betweens re-run the title's draw (a replay), so depth is re-projected with the
  vertex and captured with its word; there is no separate depth lerp.
- **Off.** The `depth` setting (default on, `PSXPORT_DEPTH`, menu "Native Depth") off passes no policy: no
  op tests, so no plane is written and the planner and rasterizer take the slot-order path unchanged.
  The menu row shows only for a title that marks a draw (`Mods::nativeDepthOffered`). Off is the
  slot-order record picture, which is not the device picture: at 4:3, ires 1, one Toasty portal frame's
  record and device VRAM (same run, same frame) differ in 1,252 of 122,880 pixels by more than 8
  levels, scattered along edges, none in the portal sky (psx-spyro issue 0224).
- **Cost.** On: one multiply per projected vertex read, one table write per stored vertex, one table
  check per guest store, one extra op per tested triangle, and per tested `Polygon` op a check and a mark of its
  native rows in the open group's map (`grouped_`); per depth group two draws and two pipeline binds. Off: an
  inline range test per store and an early return per stored vertex. A title or op that marks nothing pays no op
  words, no groups and no `gl_FragDepth`. Nothing per pixel on the CPU. Slivers (Raster, below) add groups above 1x. Spyro 1
  Stone Hill terrace (`warp 0/1/3`, `spyro-at 196200 148000 27000 1536`), 600 fields, ires 8x, 4K sink, unpaced,
  main thread (`perf stat` instructions): 236.7 G on, 213.5 G off (+10.9%; the slot-order pass before depth
  groups measured +2.7%). About 120 depth groups per record there. `perf record` on against off: the group map
  (`WrittenMap::mark`, `any`, `depthGroup`) about 9.6 G, the driver's extra draws and pipeline binds about 6 G,
  vertex depth tracking under 2 G. Spyro 2 Glimmer (`warp 0/1/0`, Up and Left held 1500 fields after a 1500-field
  arrival, Spyro running, fps60, judgement settings, unpaced, whole process): 279.2 G on, 259.2 G off (+7.7%),
  terrain, Spyro and `Model` mobys together. GPU time is not measured: SDL_GPU has no timestamp queries.
- **Check it.** Spyro 1, Toasty portal (`warp Artisans/Toasty/portal`): the rock wall meets the floor along its
  real intersection with depth on; `PSXPORT_DEPTH=0` shows the wall's slot-order overlap. Spyro,
  his toes, horns and crest draw the same with depth on and off. At the Toasty start (`warp 0/4/0`) the
  plinth keeps its skirt over the grass. At the Artisans fountain
  (`warp Artisans/Artisans/dragon 4`) the sunk pedestal keeps its rim, the pond's stepping disks meet the water at the waterline, and no far-pass river shows
  through the wall in the 16:9 margin (psx-spyro issue 0223).

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
for the displayed rect. `RenderState::buildDisplay` latches it once per present, before the present is sized, announced
or drawn, so every reader of `guestDisplay.plan()` sees this present's plan. A title's guest projection plan there keeps the retail centre and draw width
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
waiting `VSync(n)` (n = 0 or n > 1) returns one field's HBlanks (263 NTSC, 314 PAL) in `$v0`; `VSync(1)` does not wait
and returns the HBlanks since the last delivered field edge (`Timing::hSyncsSinceDisplayBoundary`, C-12 `0x800A1758`). `step` runs one
`stepFrame(Core&, frame)`; the driver calls `Game::presentation.commit(core, guestFields)` exactly once
per frame, `guestFields` being the measured guest display fields it covered (no default). The shell
aborts unless `FramePresenter::fence()` advanced by exactly one.

`GameRuntime::presentDeadline` decides who advances simulated display time and returns the present's host
deadline. The default, `FramePacer::subframeDeadline`, advances Timing, devices and the VBlank latch by the
represented field count. Every completed field raises VBlank; a VBlank the guest has not yet taken when the next field
completes is delivered first (`Hle::irqPoll`), because between turns the guest is waiting in VSync and takes each
field's interrupt, so an n-field step runs the guest VBlank handler n times. A title with its own field scheduler advances `Timing::advanceDisplayFields` per
delivered field and plans with `FramePacer::presentedFieldsDeadline`, which creates no field. An interval of n fields
ends n fields after the last delivered boundary, moved on by the whole fields the guest's work already ran past
(`EmulatedTime::advanceDisplayFields`), so every boundary lies on the field grid and the clock keeps the part field it
ran into; its parts are spaced back from that end, so the parts the guest already ran past arrive at once. A frame
driver's guest turn (`ExecutionBudget::untilFieldEdge`) runs to the next field edge, so a turn without a wait ends on
or just past that edge. In-betweens split the deadline of the same fields.

A frame driver that replaces a libetc `VSync(n)` counts `Timing::vsyncFields(n)`: VSync waits for field n-1 since its
last return, then for the next VBlank (Crash Bash `0x800320EC`), so guest work that ran past field k spans
max(n, k + 1) fields and the frame ends on the field grid. An owned loop takes n from the guest's cadence and still
counts this way: the extension is the guest clock, not a wait. Committing n alone leaves the SPU advances behind that
clock (psx-crash-bash issue 0045); the VBlanks follow the fields Timing delivers. Crash Bash's DisplayFrame override takes it. A driver whose
turn ends at the stock VSync (`StockVSync` notes the wait, `Timing::noteVSyncWait`) commits
`Timing::takeStepFields()`: the wait's fields less those earlier no-wait steps already delivered since its last return,
or, for a turn that ran out without a wait, the field edges it crossed (C-12, Vagrant Story). Toy Story 2's resident
loop commits its fixed count (psx-toy-story-2 issue 0059); the titles whose loop is not owned answer each guest VSync
with one field (each title's frame-loop issue). Spyro's field owner
needs no count: its host turn delivers each field as the guest clock crosses it. Deadlines step at the guest's field rate (`field_rate.h`: NTSC 59.817 Hz, the console's 263 lines of 3413
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
`TitleSession::setPaneHost` → `RenderState::setPaneHost`): every build (`buildDisplay`, a host picture's
`buildImage`) ends in `RenderState::finishBuild`, which counts the present and calls the host's `built`,
and `show` calls its `show` after the pane's wait. The picker attaches itself to
the focused panel, builds its own present image at `built` and shows it at `show`, so its cadence
is that panel's presents, in-betweens included. The selector is a host screen of the product: its Game
builds panes into its present image (`DisplayPresenter::buildPanes` → `RenderState::buildPanes`) and
shows it through `showPresentImage`, so the overlay, the RmlUi menu and host input are the ones a title
frame has. The picker's loop builds, paces with `FramePacer::hostScreenPace` and shows itself only on an
iteration where the focused panel did not present, and skips the pace when the panel's step already paced its fields (`title-host.md`). A pane
samples the session's newest present image (`RenderState::lastPresented`; `title-host.md` for when a
panel counts as ready).

A pane's present image is filled, not letterboxed: `present::fitPicture` (`viewport_layout.h`) centre-crops the
source to the image's shape at its displayed 4:3 aspect, a guest picture at the rendered resolution
(`PresentInputs::fill`) and a host picture (a native movie) through its sampled uv rectangle; the picture then
covers the whole image. The crop's first column joins the display origin in the row's own unit (`planPresent`): a
halfword per column at 1555, three bytes per column at 24bpp, where `disp.x` is a byte. The
selector draws each picture 1:1 at its own size, centred on its panel's cover rectangle and cut by the
seams (`PickerPanes`), so nothing is cropped to fit or magnified. A panel's `PSXPORT_PRESENT_SHOT_AT` captures its
pane image, which the selector draws only once the panel is ready (a boot prefix shows the logo over black);
`pshot` captures the window the player sees.

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
The wall-clock `presents:` line is the one real-time check; it needs a quiet host.

### Cost and cadence

`GpuPerf` (`debug/gpu_perf.*`, a stale name: it meters CPU, GPU and pacing) reports what a frame and a present cost
in units no host load moves, so two builds are compared on any host, paced or unpaced. Counting runs while the `perf`
channel is on (`PSXPORT_DEBUG=perf`, REPL `debug perf`); off, each hook is a cached branch.

- **CPU, per logic frame.** `lucent::perf::WorkClock` (Lucent `work_clock.h`) counts user-space instructions retired by
  the product's threads: the emulation thread and the `Coro` fibers it resumes (Tomba's guest tasks), which never run
  at once. Each thread reads only its own counter (`lucent::perf::ThreadWorkCounter`:
  `perf_event_open(PERF_COUNT_HW_INSTRUCTIONS)`, user space only, one event per PMU on a hybrid CPU); a `Coro`
  handoff banks the leaving thread's work. Where no
  counter opens (`perf_event_paranoid` above 2, a VM without a PMU, macOS, Windows) the unit is thread CPU time, and
  every line says so (`cpu-ms`, "NOT instructions"). One read per boundary. The frame driver's `frameBegin`,
  `markPre`, `phaseBegin`/`phaseEnd` and `frameEnd` split the frame into `pre`, `padfence`, `audio`, `game-logic`,
  `present`, `replay` (`ReplayFrame::beforeDraw`, the in-between runs of the draw section) and `other` (the rest
  after `markPre`); phases nest and an inner phase's work is not its parent's.
- **GPU, per record present.** `RecordRasterizer` counts what each `show` records (`render/gpu_work.h`): draws,
  pipeline binds, render passes, texture copies, triangles, pixels its draws cover at the internal resolution (the
  raster plan's quads, once per draw), copy pixels and bytes uploaded. These follow from the record, ires and the
  canvas. The sink pass into the present image (fixed by the sink) is not counted. GPU time is not measured: SDL_GPU
  has no timestamp queries.
- **Cadence, from the plan.** Every scheduled present (`DisplayPresenter::showAt`) is due the fields its pacer held
  it for (`GpuPerf::heldFor`) after the one before, a field lasting the guest's field period. The guest moment it
  shows is `from + t (shown - from)` in records (logic ticks). A simulated 60.00 Hz display shows at each vblank the
  newest present due by it; `cadence:` counts the logic ticks each vblank advances. A 30 Hz title at fps60 reads
  almost all `0.50`, with a `0.00` about every 5.5 s from the 59.817 Hz field on 60.00 Hz; `unknown` counts vblanks
  across a present that names no record: a movie, a load, and every present with fps60 off (`FramePresenter::commitSealed` names no shown record there). The pacer's real
  deadlines do not enter it, so it reads the same unpaced and on a loaded host.

Lines. At run end, always: `[playcheck] cadence: vblanks=… planned_presents=… unknown=… logic ticks advanced per
60.00 Hz vblank: 0.50x… 0.00x…`. With `perf` on: `[perf] cpu run-end: <frames> logic frames metered in <unit>`, one
`[perf] cpu <phase>: p50 p99 max Minsn/frame, total Minsn` line per phase and one for the whole `frame`, then
`[perf] gpu run-end: <n> record presents metered` and one `[perf] gpu <count>: p50 p99 max <unit>/present, total`
line per count. Percentiles are at most 1.6% high (`lucent::perf::CountHistogram`); max and total are exact. With
`perfframe` on, one `frame N insn: pre=… total=…` line per logic frame and one `present gpu: …` line per present,
raw counts, for diffing two runs frame by frame.

Comparing two builds follows the `perf-units` skill: the same deterministic recipe (a warp and held buttons,
`PSXPORT_NOPACE=1`, the judgement ires and sink) on each with `perf` on, at least two runs a side, and
`perf_compare.py` over the logs. Measured on Spyro 1: on one build the GPU counts repeat exactly and the CPU phase
totals within about 0.1%, whatever the host load. The meter's own cost (the boundary reads and the covered-pixel sums)
is about 0.2% of the process's user-space instructions at 8x. A `preseq` dump converts the picture on the CPU inside
the present phase, so meter runs take none. Spyro 1 Stone Hill, present phase 1x to 8x: +4.5% (draws +37%). Above
1x the planner draws slivers, rows only at S and a second walk per triangle, all per primitive (psxport issue 0199).

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

## Owner journal

`OwnerJournal` (`game.ownerJournal`) turns each interval's findings and refusals into rows, one per reason,
writer or producer and screen: `UnownedMoving`, `Port`, `Overwrote`, `ReplayRefused` and
`IntervalHeld` with its cause (`Cut`, `StaleShown`, `NoPrevious`, `ChainBreak`, `NotReplayed`). A row carries the guest function, block PC, dispatched entry, image and
generation, the OT span, a sample packet address and GP0 command, intervals seen and lost, primitives, the
screen's title-owned key and warp route (`TitleDebugMenu::screen`) and the `decomp_pipeline.py` command for
the writer. A finding with no writer (a replay refusal) names the declared producer or the section entry as the
function and resolves its image. The screen of an interval is the key `TitleDebugMenu::screenKey` read when S was
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

A bug report takes three pictures of one frame. `psx.png` is the guest draw area from the device VRAM,
captured by `BugReportSession::frameSealed` right after `FramePresenter::commit` seals the next record, so
every title produces it through the one commit path. `screen.png` is that sealed frame's first whole present
(`BugReportSession::presented`, from `DisplayPresenter::showScheduled`: the present that shows that sequence
and is not an in-between). With no sealed frame shown whole within 4 fields the newest present is the
screenshot and the report says the device render was not captured. `motion.png` is an APNG of the last second of window
presents: `PresentClip` keeps 60 layers of a 640x360 RGBA8 GPU texture array (about 55 MB of VRAM,
allocated on the first window present), letterboxes one GPU blit per swapchain present into the next
layer and records its time, and downloads the ring only when a report is written. Nothing in it is
CPU work per present, and its size does not depend on the internal resolution. The saved setting
`PSXPORT_BUG_REPORT_CLIP=0` turns it off on hardware that cannot spare the memory: no texture, no blit, and
reports carry no `motion.png`.

A report reproduces from `state.bin`, the whole machine state (`psx::state::saveToFile`) taken at the
reported frame, loaded with `PSXPORT_LOAD_STATE` on a writable copy of `memcard.mcr` (the state restores
the card it held). `repro.pad` from boot is kept, but a replay follows only the build and settings that
recorded it: Spyro 1's 2026-10-31 reports replay deterministically on a later build and still walk
elsewhere after the first gameplay phase (psx-spyro issue 0225). A title whose frame driver keeps native
state names it through `GameRuntime::nativeState`, and a state carries the active code images
(`images` section), so a fresh process loading it can dispatch the title's loaded overlays.

## Status

The native and GTE draw paths, the render queue, painter, keyed blend and every title's
`RenderCapabilities` are deleted, and so are the state producers and the frame composer: an in-between is a
replay of the title's draw section or S as drawn. `record` and `device` are the only paths. Remaining work:
depth for every title but Spyro 1, and depth for guest-code SWC2 stores.
