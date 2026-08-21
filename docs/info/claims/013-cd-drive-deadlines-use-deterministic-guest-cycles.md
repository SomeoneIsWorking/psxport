---
id: C013
kind: claim
status: holds
created: 2026-08-21
tags: timing,cdc,determinism,recompiler,interpreter
depends: runtime/recomp/timing.cpp#Timing::advanceGuestInstructionTicks, runtime/recomp/interp.cpp#interp_flat, tools/recomp/emit.py#emit_run, tests/test_interp_guest_cycles.cpp#test_interpreter_ticks_service_the_shipping_cdc_deadline, tools/recomp/test_emit.py#test_exec_loop_sum
reconfirmed: 2026-08-21
verified_at: 2026-08-21 12:45:00
---

## Claim

CD drive deadlines use deterministic executed guest instruction-time. Host wall time, process
descheduling and debugger pauses do not advance this clock, and interpreted and emitted guest
windows advance the same counter. One instruction currently contributes one tick; this is an
ordering model, not cycle-accurate R3000 timing or proof of physical 75/150-sector-per-second speed.

## Evidence

`test_exec_basic_alu` runs a four-instruction emitted window and reports four ticks.
`test_interp_guest_cycles` runs the same four guest instructions through the shipping interpreter
and reports four ticks; its second case reaches a CDC deadline at tick four and leaves INT1 pending.
The path-sensitive emitted loop reports 23 executed instruction ticks rather than the seven-instruction static
body answer, proving backward branches count each executed iteration.

Vagrant Story's live first ReadN armed at guest tick 83,098,580 with deadline 83,324,372 and serviced
at 83,324,373: the nominal double-speed 225,792-tick threshold plus one batched-instruction overshoot.
Every following event used the same instruction-time delta. Crash Bash independently armed at 1,331,915,
returned from ReadN before the first event, then serviced LBA35799..35987 at 225,792-tick deadlines.
Neither production path contains a CDC worker, sleep or steady-clock read.

## What would falsify it

Pausing under GDB or descheduling the process changes `Timing::guestInstructionTicks`, first-INT1 ordering or a
drive deadline for the same guest instruction stream; an interpreted and emitted copy of the same
window reports different instruction ticks; a loop counts its static body rather than executed
iterations; or any CDC sector event is armed from host wall time. Claiming physical drive-rate or
cycle accuracy before issue 0007 is resolved also falsifies the scope of this claim.
