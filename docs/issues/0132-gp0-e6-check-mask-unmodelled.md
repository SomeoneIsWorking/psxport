# 0130 — GP0(0xE6) check-mask is decoded and never honoured, so overlapping prims composite twice

**State:** recorded, not fixed. Found 2026-09-28 while making the GP0 command word readable; a
readability refactor must not change what the code does, so the gap is left exactly as it was.

## What the guest asks for

GP0(0xE6) sets two bits in the low byte of its word:

- bit 0 — *set mask*: a pixel this primitive draws has its mask bit set;
- bit 1 — *check mask*: **skip the write wherever the destination pixel's mask bit is already set.**

On hardware the second bit is a real compositing rule, not a filter: a primitive drawn with check-mask
on does not double-composite over a region it has already written. A game that draws a sprite and then
its highlight with check-mask on gets ONE composite; a port that ignores the bit gets two, and the
overlap is brighter than the artist authored.

## What this framework does

`Gp0Command::maskBits()` (`runtime/psx/gp0_command.h`) decodes both bits and both are stored. Neither
is used as a pixel test. The rasterizer's `put_px_b` (`runtime/psx/gpu_native_raster.cpp`) writes every
pixel unconditionally.

The file has carried a `PSXPORT_DEBUG=maskbit` probe for this since before the decode was named, and
that probe is the only thing that looks at the bits. Its comment already said "Neither is modelled.
Bit 1 matters for correctness" — that was correct then and is correct now.

## Why it was not fixed here

The fix is a per-pixel test in the rasterizer's innermost write path, on every port's rendering path.
That is a behaviour change on the path a readability refactor must not touch, and it cannot be
qualified by this change: there is no hermetic test that can say a given pair of overlapping prims
*should* have composited once, because "should" is the guest's intent, which is exactly what is missing
from the framework.

## What would settle it

1. Run the existing `PSXPORT_DEBUG=maskbit` probe on the titles and read the COUNT. A measured non-zero
   makes this a live defect on that title; a measured zero across every title makes it a latent one.
   The probe exists precisely so that answer is a measurement rather than an absence of output.
2. For a title that enables it, take a frame and a `PSXPORT_PRIMDUMP` of the overlapping pair, and
   compare the composited pixels against a console capture. Only that decides whether the double
   composite is visible.

## Where

- `runtime/psx/gp0_command.h` — `Gp0MaskBits`, `Gp0Command::maskBits()`: the decode, and the comment
  recording why it exists.
- `runtime/psx/gpu_native_raster.cpp` — `GpuState::put_px_b`: the write with no mask test.
- `runtime/psx/gpu_native.cpp` — the `PSXPORT_DEBUG=maskbit` probe that measures whether any title
  sets the bit.
