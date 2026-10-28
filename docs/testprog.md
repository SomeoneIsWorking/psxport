# Framework test program

A freestanding PS-X EXE built from `testprog/guest/testprog.c` with `clang --target=mipsel-unknown-none-elf`
(no SDK, no game assets) and run through the product as the title `psxtest_port`. It checks the owner
ledger, the interval verdict, the owner journal and the present cadence against draws whose owners it knows.

## Build

`cmake/testprog.cmake` (top-level psxport builds only) runs `tools/testprog/build_testprog.py`: it compiles
the one C file, links the object itself (alloc sections from `0x80010000`, `.text.0start` first, `.bss`
zero-filled, the o32 `R_MIPS_32/26/HI16/LO16` relocations) and writes the EXE to
`build/agent/testprog/psxtest/PSXT_000.01` with `testprog_identity.h`, the hash the product admits it by.

## Guest

The guest publishes its facts at `0x8001000C` (VSync, `square_draw`, the OT, the sine table, the frame
counter, fields per frame, the scene words and the display buffer), double-buffers a 16-bucket OT cleared
by DMA6 and submitted by DMA2, and calls VSync once per field. Each scene runs 240 frames, then the next:

| scene | drawer | expected rows |
|---|---|---|
| `owned-square` | `square_draw`, the producer's object; frames alternate 2 and 3 fields | none; presents at thirds and halves |
| `unowned-moving` | `drifter_draw`, a TILE no producer declares | `UnownedMoving` for `drifter_draw` |
| `mmio-port` | `mmio_bar_draw`, a TILE written to GP0 | `Port mmio-gp0` for `mmio_bar_draw` |
| `block-dma-port` | `dma_bar_draw`, a TILE sent by block DMA2 | `Port block-dma` for `dma_bar_draw` |
| `reused-slot` | `square_draw` on a slot a new object takes every 12 frames | none in this stage |
| `amended-square` | `square_draw`, then `amend_draw` stores the packet's colour word before submit | `Overwrote` for `amend_draw`, producer `square_draw` |

One more scene runs only when requested and never comes round in the cycle. It must end the session with a
`[render:error] draw-list cycle` line, the player's text and exit status 3, and the frame it was drawn in never
presents:

| scene | drawer | expected end |
|---|---|---|
| `looping-ot` | `looping_ot_draw`, a full-screen semi-transparent TILE in bucket 8 with bucket 3 linked back to bucket 10 | draw-list cycle, refused by `OrderingTableCursor::advance` |

`planted <scene>` on the control channel requests a scene from the next frame (`planted looping-ot`).

`backdrop_draw`, a still TILE every scene draws without an owner, must never make a row. Besides the rows
above the journal holds, by design:

- one `IntervalHeld/stale-shown` and one `IntervalHeld/no-previous` row on `planted owned-square` from the first
  presents after boot, before a drawing record and a shown predecessor exist;
- at every scene change, one held interval for the drawer of the scene that ended (it vanished from S), on the
  row of that drawer's reason with the new scene as its screen, since S belongs to the new scene. A run that
  cycles the scenes k times has about k such intervals per scene change.

The planted cases cover the stage 2 reasons the framework can raise today (`UnownedMoving`, `Port`,
`Overwrote`). `Baked`, `ReplayRefused`, `PlacelessVertex`, `ClosureFailed` and `BranchChange` have no case: the
first two need a texture-feedback upload and a guest redraw the program does not yet make, the last three are
reasons of stages 3 and 4 that the framework does not raise yet. The reused slot is pinned only by
incarnations (stage 4).

## Host

`testprog/host/` is the title: `Runtime` binds VSync and the square's producer from the facts, `FrameDriver`
runs fields until the frame counter moves and commits once with the guest's field count, `SquareProducer`
renders the square at any t from its saved fields and the guest sine table, and `DebugMenu` names the scene
as the journal's screen with `planted <name>` as its route.

## Check

    CC=clang CXX=clang++ uv run --frozen python tools/build.py --build build/agent
    PSXPORT_NOPACE=1 PSXPORT_SHOW_WINDOW=0 PSXPORT_DEBUG=presentseq build/agent/bin/psxtest_port

then `owners` on the control channel (or the JSONL under `PSXPORT_OWNERS_DIR`) lists the rows above and the
by-design rows, and `presentseq` shows the owned square at `t` = 1/3, 2/3, 1 and 1/2, 1 alternating, never
`t = 0`; the run-end `records:` line counts at most one `stalled` present per switch into the alternating
cadence. On 2026-10-27 a 60 s unpaced run gave exactly that: 10 rows (4 planted, 4 scene-change, 2 boot) and
`stalled=0`.
