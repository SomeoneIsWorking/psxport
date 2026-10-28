# 0192 — A cyclic draw list reached the GPU

Status: fixed (2026-10-28). Owner: `runtime/psx/gpu/draw_list_cycle.*`, called by `gpu/ordering_table.cpp`,
`present/packet_chains.cpp` and Spyro's `render/terrain/guest_depth_bins.cpp`.

## Observed
Mega Man X4 issue 0043: a scheduler bug linked one fade packet into its own ordering table twice, so the DrawOTag
chain cycled `0x8012F498 -> 0x80169D78 -> 0x8012F498`. `OrderingTableCursor::advance` stopped only at its
0x10000-node cap, the record took 32,763 full-screen semi-transparent rectangles, and one `record` submit hung the
amdgpu ring. A console's DMA2 walk never leaves such a list, so the game hangs there; the port instead truncated it
and drew the truncation.

## Cause
`gpu/ordering_table.cpp:OrderingTableCursor::advance` bounded a walk by a node count (`kOtNodeLimit`) instead of
detecting the revisit, and drew everything up to the cap. `PacketChainWalker` and Spyro's depth-bin walk bounded
cycles the same way.

## Fix
`LinkCycleCheck` (Brent's check, no memory) is the cycle test of every linked walk: the DrawOTag walk,
`PacketChainWalker::walk` and Spyro's `assignPair`. A revisit calls `refuseDrawListCycle`, which latches the
presentation device off with `FaultCause::DrawListCycle` (`RenderDevice::latchFault`, 0191) and the walk stops
there. The latch disables the device before the frame is sealed, so nothing of that frame is submitted; the session
ends at its next step (`TitleSession::End::DrawFault`), the player gets the error box "The game asked to draw a list
that never ends, which hangs a PlayStation, so it will close." with the node, the refusing walk and the guest
writer, and the process exits with status 3. `kOtNodeLimit` is deleted; `PacketBound::anywhere()` replaces Spyro's
`count(kOtNodeLimit)`.

The writer named is the writer of the edge into the revisited node that Brent's check lands on, an edge of the
cycle but not necessarily the store that closed it. When that block's function is not on the shadow call stack the
line says so instead of printing `fn 0x00000000`.

Withdrawn from the first version of this fix (review 2026-10-28):

- A packet ending inside a poly-line is not refused. The console's GPU reads the next words as vertices until a
  `0x5xxx5xxx` word arrives, which gpu.c does too: defined behaviour that finishes, at worst with stray lines.
- `FrameRecord::kMaxEntries` and `kMaxPoolHalfwords` are the port's storage, not console limits. Past them a record
  is marked incomplete again, as before, now with one `[record:warn]` line; it is never replayed.

## Shown
Test program (`build/agent/bin/psxtest_port`, hidden, unpaced, ires 0, sink 3840x2160, `planted looping-ot`;
`scratch/player-bugs-8/r3-looping-ot`): `[render:error] draw-list cycle at 0x80010DA4, refused by
gpu/ordering_table.cpp:OrderingTableCursor::advance: node 0x80010DA8 links back to it after 24 nodes; guest writer
block 0x80010B1C (its function is not on the shadow call stack) wrote 0x80010DA8`, then the player's text line,
the run-end report and exit status 3. The presents after the latch show earlier records (`shown=2 from=1`), never
the cyclic frame's record, and a disabled device submits nothing. Block 0x80010B1C lies in `cstart`
(`testprog.o` `.text` at 0x80010040, `cstart` at +0x730), which the entry stub reaches by `j`, so no call frame
names it.

All 11 products rebuilt with Clang on the committed sources, one paced 2400-frame hidden run each (fps60 1, ires 0,
sink 3840x2160, `PSXPORT_DEBUG=presentseq`); Spyro, Crash, Tomba and Spider-Man as the shipped picker product with
`pick <slug>` (`r3-paced`, `r3-picker`, and `r3-quiet`/`r3-picker-quiet` again at load under 2). No run has an
`:error]` line or a fault. `missed` counts an interval past its held fields plus half a vsync:

| product | load | p99 | missed | the long intervals |
|---|---|---|---|---|
| spyro (pick spyro1) | 4.70 | 17.5 ms | 5 | boot, guest call `0x8001ED5C` outliving a turn |
| toystory2 | 4.26 / quiet | 100.5 ms | 6 / 8 | movie frames held 4-6 fields by the FMV clock (100 ms cadence, ts2 0058) arriving 206-246 ms late at the movie's start; one boot overlay load (`MEMORY.BIN`) |
| crashbash | 5.36 / quiet | 33.5 ms | 4 / 7 | p99 is the 30 Hz boot screens (288 two-field presents, not misses); misses: boot (crashbash 0045), and 0193 |
| crash (pick crash1) | 5.30 / 1.74 | 41.5 / 17.0 ms | 116 / 1 | 0193 |
| ctr | 3.37 / quiet | 17.5 / 17.0 ms | 3 / 2 | widescreen-owner resolve at fence 446, one disc read callback |
| tomba2 (pick tomba2) | 8.54 | 33.5 ms | 2 | p99 is the intro movie's 30 Hz frames (2091 two-field presents, `[fmv]`, not misses); the 40 s never leaves the movie, so no in-betweens |
| spiderman (pick spiderman1) | 5.15 | 17.0 ms | 0 | |
| vagrant, megamanx4, tekken3, c12 | 3.1-4.0 / quiet | 17.0 ms | 0-1 | one boot present on tekken3 |
