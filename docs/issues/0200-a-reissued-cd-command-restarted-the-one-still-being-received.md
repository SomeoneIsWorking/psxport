# 0200: a re-issued CD command restarted the one still being received

Status: fixed

## Observed
Spyro 2, standalone or as the focused selector panel. After the first attract demo the title starts its
next load (state 5, level 0) and never leaves it: the panel shows the logo over black from then on.
`0x800698EC` stays 0, `0x800682E8` is 0, `0x800682F4` (XA music state) stays nonzero.

## Cause
Spyro 2's read poll `FUN_80013690` reports busy while the music machine `FUN_80012CBC` is not idle. Its
stop case 9 sends `CdControlF(Pause)` (`0x80058994`) each time `CdSync(1)` (`0x80058810`) returns 2, until its
libcd ISR's status byte `0x8006670C` drops bit 0x20. The stock `CdSync` answers 2 at once, so the guest sends
Pause on every poll, and `runtime/psx/cd/cdc_native.cpp:CdController::issueCommand` rescheduled the pending
Pause from its register write each time. It never executed and the status stayed `0x22`. Real libcd waits for
BUSYSTS to clear before it writes a command, so the earlier Pause would have executed first. The old Spyro 2
driver hid this by jumping emulated time to the field edge between turns.

`CdSync(1)` answering CdlNoIntr while the command is in flight is not the fix: in a title whose libcd is
replaced whole (Spyro 1) nothing acknowledges the first response, the command never completes and the guest
spins forever.

## Resolution
`issueCommand` first runs a command still in its receive or execution phases through its execution
(`retirePendingCommand(RetireTo::Execution)`), then schedules the new one. `beginRead` still retires through
completion.
