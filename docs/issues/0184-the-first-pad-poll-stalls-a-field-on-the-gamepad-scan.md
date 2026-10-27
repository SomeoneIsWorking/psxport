# 0184 — The first pad poll stalls a field on the gamepad device scan

Status: closed (2026-10-27). Owner: `runtime/psx/input/host_input.cpp` (`psx::input::HostInput`).

## Observed
Tekken 3, 40 s paced hidden run from boot: `worst=427ms beyond_range=1`, where the run on psxport `79dcd0e6` had
`worst=22.53ms`. A 4 s run (`psx-tekken-3/scratch/player-bugs-2/t3freeze/run.log`) shows 372-460 ms between guest call
`0x800B0548` returning and the next field; before, 7 ms.

## Cause
`HostInput::rescanControllers` brought SDL's gamepad subsystem up lazily, on the first pad poll of the run, inside a
field. `SDL_InitSubSystem(SDL_INIT_GAMEPAD)` enumerates HID devices through libusb/udev, which took 370-460 ms on this
host. Thread stacks sampled during the stall (`t3freeze/stack/stacks.txt`): `udev_enumerate_scan_devices` <
`libusb_init_context` < `HIDAPI_JoystickInit` < `SDL_InitSubSystem` < `HostInput::rescanControllers` <
`HostInput::poll` < `Pad::serviceFrame` < `tekken3::frame::FiniteFrame::runBarrier`. A shown window always paid this
on the first poll; hidden runs skipped controller handling until `6c01757c` (injected virtual pads), which is why the
tool runs only now show it.

## Fix
`HostInput` brings the gamepad subsystem up in its constructor, before the first field, and its destructor closes its
pads and releases the subsystem.

## Proof
Same 40 s paced hidden Tekken 3 run (`t3freeze/fixed/run.log`, load 9): `worst=23.40ms missed=0 beyond_range=0`; the
gap after `0x800B0548` returns is 7 ms.
