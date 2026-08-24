---
id: C012
kind: claim
status: holds
created: 2026-08-21
tags: cdc,cdrom,readn,dma
depends: runtime/recomp/cdc_native.cpp#cdc_drive_service, runtime/recomp/timing.cpp#Timing::advanceGuestInstructionTicks, tools/recomp/emit.py#emit_run, tests/test_cdc_continuous_read.cpp#test_first_sector_waits_one_drive_period
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:52:59
---

## Claim

The CDC continuous ReadN model schedules first and following sectors in deterministic guest
instruction-time. It uses nominal thresholds derived from the PSX CPU clock and the Setmode-selected
75/150-sector-per-second rates. Sector arrival is independent of BFRD FIFO drainage; BFRD can only
expose or swap a sector whose drive deadline has elapsed. Because one executed instruction currently
contributes one tick, this claim establishes deterministic ordering, not cycle-accurate physical
drive timing; issue 0007 records that limitation.

## Evidence

`test_cdc_continuous_read` drives the shipping begin/read/write/DMA/service path with an injected
instruction-time counter: 5/5 tests and 59 checks. At tick 0 and deadline-1 it sees no data/INT1; at
the nominal 225,792-tick double-speed threshold it sees the first LBA16 sector and status 0x22. A 2060/2340-byte partial
FIFO remains intact while LBA17 becomes ready only at the next deadline; Pause returns 0x22 ACK,
0x02 completion and cancels the event. `test_interp_guest_cycles` executes the actual interpreter
and proves its four-instruction window services the shipping CDC deadline; the emitter's path-sensitive
loop test reports 23 instruction ticks rather than the static-body answer 7.

Live Vagrant Story arms at tick 83,098,580 and services at 83,324,373 for a 225,792-tick deadline
plus one batching-tick overshoot. Its 17th callback queues and dispatches Pause before another sector.
Five ReadN transfers complete: four WAVE loads and the 271-sector TITLE.PRG transfer; the sixth
`DsEndReadySystem` call is pre-read initialization. Crash Bash returns from a 189-sector start before its first INT1, then
services LBA35799..35987 at the same nominal 225,792-tick period and prints `done loading`. Crash Bash's
stricter true-oracle completion-state comparison still refuses the port because its transient
completed-pending result is cleared before the guest observes it; issue 0006 records that separate
HookEntryInt/ReturnFromException ordering boundary rather than attributing it to drive pacing.

## What would falsify it

A host pause/debugger stop changes sector ordering; ReadN emits its first INT1 before one drive
period; a partial FIFO suppresses a due event or changes before BFRD service; Pause leaves a live
deadline; single/double speed differs from the nominal 451,584/225,792 tick thresholds; interpreter
and emitted execution advance different instruction counts; or a real consumer cannot arbitrate
Pause before the next sector.

## Re-confirmed 2026-08-21

Reconfirmed on the deterministic-cycle implementation with the hermetic both-answer gates and live
Crash Bash/Vagrant Story consumers above. The final Clang CTest suite passes 77/77.

## Re-confirmed 2026-08-21

Post-landing Clang CTest passed 77/77; deterministic deadline tests passed and Toy Story 2, Vagrant Story, and Crash Bash all advanced past their former synchronous or stalled ReadN boundaries.

## Re-confirmed 2026-08-22

Post-commit full Clang CTest passed CDC continuous-read coverage and emitter execution controls within 85/85.

## Re-confirmed 2026-08-22

Post-composition Clang CTest 90/90 passed continuous-read, CDC drive-rate, instruction parity, and emulated-time controls.

## Re-confirmed 2026-08-22

Combined Clang framework build and full 91/91 CTest pass at f468f2c7, including CDC deterministic-time controls, HSync counter coverage, and DMA regression coverage.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 1e3afdfb: CDC continuous/read timing tests passed in the full 99/99 suite; emitter passed 49/49.
