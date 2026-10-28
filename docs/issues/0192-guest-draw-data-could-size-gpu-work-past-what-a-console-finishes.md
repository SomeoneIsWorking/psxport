# 0192 — Guest draw data could size GPU work past what a console finishes

Status: fixed (2026-10-28). Owner: `runtime/psx/gpu/record_fault.*`, called by `gpu/ordering_table.cpp`,
`present/frame_record.cpp` and `present/packet_chains.cpp`.

## Observed
Mega Man X4 issue 0043: a scheduler bug linked one fade packet into its own ordering table twice, so the DrawOTag
chain cycled `0x8012F498 -> 0x80169D78 -> 0x8012F498`. `OrderingTableCursor::advance` stopped only at its
0x10000-node cap, the record took 32,763 full-screen semi-transparent rectangles, and one `record` submit hung the
amdgpu ring. A console hangs on the same list; the port instead truncated it and drew the truncation.

## Cause
Every limit the record path had on guest data was a silent cap: the OT walk's `kOtNodeLimit` cut a cyclic chain at
65,536 nodes and drew them; `FrameRecord::append`, `appendClut` and `appendUpload` marked a record past
`kMaxEntries` (262,144) or `kMaxPoolHalfwords` (8 M) incomplete and went on; a packet that ended inside a poly-line
was not seen at all (the device read the next packets as vertices). `PacketChainWalker` and Spyro's depth-bin walk
bounded cycles by a count instead of detecting them.

Not caps, and left as they are: GP0 coordinates, sizes, texture page and CLUT fields are hardware-width bit fields
that gpu.c and the record decode mask exactly as the console does (a fill is at most 1024x512, a copy or upload
wraps), so guest data cannot make them larger.

## Fix
`gpu/record_fault.*` is the one refusal: `raiseRecordFault(kind, guest address, refusing layer, detail)` logs one
`[record-fault:error]` line and aborts, inside the guest's DMA2 walk or the record append, so the record holding
the work is never sealed and nothing of it reaches the GPU. `LinkCycleCheck` (Brent's check, no memory) is the
cycle test of every linked walk:

- `OrderingTableCursor::advance`: a revisited node is `draw-list-cycle`, naming the node, the node that links back
  to it and that node's guest writer (`guestWriterOf`, the owner ledger). `kOtNodeLimit` is deleted; with no
  revisit a chain is bounded by main RAM.
- `submitOrderingTable`: a node whose words leave the device inside a poly-line (`GpuDevice::polyLineOpen`, gpu.c
  `INCMD_PLINE`) is `poly-line-unterminated`.
- `FrameRecord::append`, `appendClut`, `appendUpload`: past the bounds is `record-entries` or `record-pool`.
- `PacketChainWalker::walk` checks each chain for a cycle; `PacketBound::anywhere()` replaces Spyro's
  `count(kOtNodeLimit)`, and Spyro's `assignPair` uses the same check.

## Shown
Test program (`build/agent/bin/psxtest_port`, hidden, unpaced, ires 0, sink 3840x2160;
`scratch/player-bugs-8/`):

- `looping-ot`: `[record-fault:error] draw-list-cycle at 0x80010E34, refused by
  gpu/ordering_table.cpp:OrderingTableCursor::advance: node 0x80010E38 links back to it after 24 nodes; guest writer
  fn 0x00000000 block 0x80010B98 wrote 0x80010E38`; the last `presentseq` line is the frame before (`loop.log`).
- `unterminated-poly-line`: `[record-fault:error] poly-line-unterminated at 0x8001169C, refused by
  gpu/ordering_table.cpp:submitOrderingTable: its 4 words end with no 0x5xxx5xxx terminator; guest writer fn
  0x800106E8 block 0x800106E8 wrote 0x800116A0` (`polyline.log`).
- The six cycled scenes ran 26,000 frames with no fault (`loop.log`, before the request).

In both cases the process ends there (exit 139: the abort reaches the watchdog's crash handler) and the record that
held the walk is never sealed, so nothing of it is submitted.

All 11 products, one paced 2400-frame hidden run each, fps60, ires 0, sink 3840x2160 (`paced40`, and `paced40exe`
for the four selectors run with their first title's executable): every run exits 0 with no `record-fault`, GPU
fault or `:error]` line. Load 1-14 during the runs; p99 17.0 ms on spyro1, crash1, spider-man1, vagrant, x4, tekken3
and c12, 20.8 ms on ctr, 33.5 ms on crashbash and tomba2, 100.5 ms on toystory2 (9 missed presents at load 7.5).
