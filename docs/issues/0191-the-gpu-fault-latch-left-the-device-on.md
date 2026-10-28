# 0191 — The GPU-fault latch left the device on

Status: fixed (2026-10-28). Owner: `runtime/psx/render/render_submit.cpp` (`RenderDevice::latchFault`).

## Observed
After a `record` submit returned `VK_ERROR_DEVICE_LOST` (Mega Man X4 issue 0043), the process kept presenting. Its
DRM memory went from 0.9 GB VRAM / 0.15 GB GTT to 12.3 GB VRAM / 8.0 GB GTT in eight seconds
(`/proc/<pid>/fdinfo`, `psx-mega-man-x4/scratch/player-bugs-6/ring/base1/mem.tsv`), and at 2026-10-28 07:56:53 the
kernel OOM killer killed an unrelated qemu VM of the user's.

## Cause and fix
`latchFault` set `faulted_`, which nothing read; `enabled()`, the gate every present and capture checks, stayed
true. Each later present acquired, recorded and submitted into the lost device, whose work never retires, so SDL
cycled a fresh transfer buffer and command buffer every present. The latch now calls `RenderDevice::disable()`.

## What the player sees after a fault
The window keeps the last picture it was shown and never changes again. The guest keeps running on its pacer, so
audio and pad input go on, but nothing draws, including the ESC menu, and nothing tells the player the renderer
stopped; closing the window ends the process. The log has the `GPU fault at ...` error and the marker below.

## Shown
One forced loss (the X4 build without its 0043 fix, `scratch/player-bugs-6/ring/latch1`): after the fault at 19 s
the process held 0.80 GB VRAM / 134 MB GTT, flat to the end of the 35 s run; the run log and kernel log show no OOM.
