# 0189 — A recorded GPU fault refused every later launch

Status: fixed (2026-10-28). Owner: `runtime/psx/render/render_submit.cpp` (`RenderDevice::reportRecordedFault`).

## Observed
`~/.config/psxport/gpu_fault` held `where: record / detail: vkQueueSubmit VK_ERROR_DEVICE_LOST`, written 2026-10-28
02:40 local. Every title then exited with code 3 before opening a window, the reason printed only to a terminal the
player never sees, until someone set an override variable.

## Cause and fix
`faultPreflight` refused the GPU in every later process while the marker existed. A lost device in one process says
nothing about the next one's context: the kernel resets the ring (`journalctl -k`: "ring gfx_0.0.0 timeout ...
Process megamanx4_port", then "ring reset succeeded"), a context that did not hang is told it lost its device too,
and the marker cannot name the process that hung. The in-process latch still turns the renderer off for the process
that faulted; the marker is still written, and the next process logs it as a warning and starts normally. The
override variable is deleted.

## Shown
Every oracle and product run of this pass started with the marker present (it is the user's file and was kept),
logging `an earlier run recorded a GPU fault in .../gpu_fault: ...`.
