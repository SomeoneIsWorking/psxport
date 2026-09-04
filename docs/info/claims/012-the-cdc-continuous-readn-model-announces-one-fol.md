---
id: C012
kind: claim
status: holds
created: 2026-08-21
tags: cdc,cdrom,readn,dma
depends: runtime/psx/cdc_native.cpp#cdc_drive_service, runtime/psx/timing.cpp#Timing::advanceGuestInstructionTicks, tests/test_cdc_continuous_read.cpp#test_first_sector_waits_one_drive_period
reconfirmed: 2026-08-25
verified_at: 2026-08-25 01:16:02
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
instruction-time counter. At tick 0 and deadline-1 it sees no data/INT1; at
the nominal 225,792-tick double-speed threshold it sees the first LBA16 sector and status 0x22. A 2060/2340-byte partial
FIFO remains intact while LBA17 becomes ready only at the next deadline; Pause returns 0x22 ACK,
0x02 completion and cancels the event. The execution-counter contract attributes guest instruction
ticks at the CPU boundary so translated blocks and bounded fallback contribute to the same deadline.

Live Vagrant Story arms at tick 83,098,580 and services at 83,324,373 for a 225,792-tick deadline
plus one batching-tick overshoot. Its 17th callback queues and dispatches Pause before another sector.
Five ReadN transfers complete: four WAVE loads and the 271-sector TITLE.PRG transfer; the sixth
`DsEndReadySystem` call is pre-read initialization. Crash Bash returns from a 189-sector start before
its first INT1, then services LBA35799..35987 at the same nominal 225,792-tick period and prints
`done loading`. Issue 0006 records its separate HookEntryInt/ReturnFromException ordering boundary.

## What would falsify it

A host pause/debugger stop changes sector ordering; ReadN emits its first INT1 before one drive
period; a partial FIFO suppresses a due event or changes before BFRD service; Pause leaves a live
deadline; single/double speed differs from the nominal 451,584/225,792 tick thresholds; or a real
consumer cannot arbitrate Pause before the next sector.

## Re-confirmed 2026-08-21

Reconfirmed on the deterministic-cycle implementation with the hermetic both-answer gates and live
Crash Bash/Vagrant Story consumers above. The final Clang CTest suite passes 77/77.

## Re-confirmed 2026-08-21

Post-landing Clang CTest passed 77/77; deterministic deadline tests passed and Toy Story 2, Vagrant Story, and Crash Bash all advanced past their former synchronous or stalled ReadN boundaries.

## Re-confirmed 2026-08-22

Post-commit Clang CTest passed CDC continuous-read coverage.

## Re-confirmed 2026-08-22

Post-composition Clang CTest passed continuous-read, CDC drive-rate, and emulated-time controls.

## Re-confirmed 2026-08-22

Combined Clang framework build and full 91/91 CTest pass at f468f2c7, including CDC deterministic-time controls, HSync counter coverage, and DMA regression coverage.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 1e3afdfb: CDC continuous/read timing tests passed in the full 99/99 suite; emitter passed 49/49.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 73bb5338: CDC command phases, continuous ReadN, emulated time, DMA depletion, and BFRD split tests passed in the full 100/100 CTest suite.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 64f5e181: CDC phases and continuous ReadN tests passed within the complete 102/102 CTest suite.
