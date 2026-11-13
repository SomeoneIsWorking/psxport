# 0203: a load leaves internal-resolution pixels and the SPU capture buffers unrestored

Status: open

## Observed
Spyro 1, `warp 0/1/0` (Stone Hill start), state saved 4 s after arrival, loaded in a fresh process under
`PSXPORT_REPL=1`; judgement settings (`ires=0`, `fps60=1`, sink 3840x2160, `PSXPORT_NOPACE=1`).

- Picture: presents 1, 2, 3, 10 and 60 after the load differ from the saving run's in one spot, 33x5 sink pixels at
  (1914, 586), the ground shadow under the distant dragon statue: the saving run draws it on present 1, the loaded
  run does not; by present 60 both draw it at different widths (26x4 differing). Every other pixel matches.
- Audio: states saved 60 frames after the save and 60 frames after the load differ in `beetle.spu` only in SPU RAM
  `0x000..0x7FF`, the capture buffers (CD audio left/right, voices 1 and 3): 1951 bytes, every quarter.

Two loads of one state match each other exactly. Tomba! 2 (`warp 13/0`, `2/0`) and Mega Man X4 (`warp 0/0/2`,
frame 3000) match their saving runs on every present compared.

## Ruled out
- Guest state: RAM, CPU, scratchpad, GTE, timing, CDC, DMA, HLE, card and title sections are identical at present 60.
- GPU device: every Beetle GPU field and VRAM match at present 60 since the scanline clock is carried (`57267c51`).
- Records: the GPU section's records match byte for byte except `DrawPrimitive::owner`, an `OwnerLedger` handle a
  load rebuilds and only `present/ownership_verdict.cpp` reads.
- Depth: `RecordRasterizer` clears each plane's depth per record; Spyro's `DepthTest` marks are registered at install.
- Scale: at `ires=1` the saving and loaded runs match on presents 1, 2, 3, 10 and 60 (0 pixels), so the picture
  difference is host pixels at internal resolution that the redraw of the two carried records does not rebuild.
- SPU clock: every SPU register matches at present 60 (`CWA`, `NoiseDivider`, `ReverbCur`) since `SpuDevice`'s field
  run is carried (`57267c51`).

## Next
Picture: find which host pixels the shadow's draw reads or lands on that no carried record wrote (the chained image
under it, or a texture region drawn at scale before the save), then carry or rebuild that region.
SPU: find what feeds the capture writes differently with identical registers (CD audio input from the CDC, voice
output) by comparing the capture rings sample by sample after the load.

## Evidence
`psx-spyro/scratch/statefix/`: `x_s/A.bin` (8x save), `spot.png` (spot at native, top, and 8x, bottom; saving run
left), `pair1.png`, `pair60.png`, `i1_s/`, `i1_l/` (native pair). Scripts in `psx-tomba/scratch/statefix/`:
`saveshots.sh` (`REPO`, `BIN`, `EXE`, `WAITRE`, `SAVE_END=1` saves `B.bin` at present 60) and `replload.sh` (saves
`C.bin` at present 60); the section diff splits both files by the `PSXSTATE` section table.
