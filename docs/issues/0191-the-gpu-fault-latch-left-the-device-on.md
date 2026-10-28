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
Until 2026-10-28 the window kept its last picture, the guest ran on, nothing drew (the ESC menu included) and nothing
told the player. Now the session ends at its next step (`TitleSession::End::DrawFault`; the picker's
`Outcome::DrawFault`), `ProductHost::endOnDrawFault` shows an error box over the window, "The graphics device failed
and the game cannot draw any more, so it will close." with `GPU fault at <where>: <detail>`
(`RenderWindow::showError`; a tool's unmapped window logs it instead), and the process exits with status 3. The
control channel's `gpufault` latches the device without a device failure (`RenderDevice::latchFault` with
`FaultCause::Device`; only a failed submit or fence writes the marker, `failDevice`). A refused draw-list cycle
(0192) latches with `FaultCause::DrawListCycle` and takes the same path with its own text.

Shown (test program, hidden, unpaced, `gpufault` 2 s in; `scratch/player-bugs-8/gpufault.log`): `[render:error] GPU
fault at control channel ...`, then `[render:error] psxport test program: The graphics device failed ... GPU fault at
control channel: latched through the control channel`, the run-end report, exit status 3. The error box over a shown
window is not shown by an agent run (no visible windows); its call is the SDL message box with the window as parent.
Not shown either: the path from a real submit failure (`failDevice`), since forcing a device loss on this desktop is
banned; it reaches the same `latchFault` the `gpufault` verb and the 0192 refusal (a real, non-synthetic trigger,
`scratch/player-bugs-8/r3-looping-ot`) both drive.

## Shown
One forced loss (the X4 build without its 0043 fix, `scratch/player-bugs-6/ring/latch1`): after the fault at 19 s
the process held 0.80 GB VRAM / 134 MB GTT, flat to the end of the 35 s run; the run log and kernel log show no OOM.
