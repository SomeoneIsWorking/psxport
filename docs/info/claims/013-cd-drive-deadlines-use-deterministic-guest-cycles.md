---
id: C013
kind: claim
status: holds
created: 2026-08-21
tags: timing,cdc,determinism,recompiler,interpreter,display
depends: runtime/recomp/emulated_time.cpp#EmulatedTime::advanceDisplayFields, runtime/recomp/timing.cpp#Timing::advanceGuestInstructionTicks, runtime/recomp/frame_pacer.cpp#gpu_pace_subframe_fields, runtime/recomp/interp.cpp#interp_flat, tools/recomp/emit.py#emit_run, tests/test_cdc_emulated_time.cpp#test_yield_heavy_loop_reaches_the_same_shipping_cdc_deadline, tests/test_interp_guest_cycles.cpp#test_interpreter_ticks_service_the_shipping_cdc_deadline, tools/recomp/test_emit.py#test_exec_loop_sum
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:33:46
---

## Claim

CD drive deadlines use deterministic emulated CPU time. Interpreted and emitted guest windows
advance the same owner, and a delivered display field advances it to the hardware-derived field
boundary when the guest yielded before enough instructions executed. Host wall time, process
descheduling, debugger pauses, and the host-pacing switch do not define this clock. One instruction
currently contributes one tick; this is an ordering model, not cycle-accurate R3000 timing or proof
of physical 75/150-sector-per-second speed.

## Evidence

`test_exec_basic_alu` runs a four-instruction emitted window and reports four ticks.
`test_interp_guest_cycles` runs the same four guest instructions through the shipping interpreter
and reports four ticks; its second case reaches a CDC deadline at tick four and leaves INT1 pending.
The path-sensitive emitted loop reports 23 executed instruction ticks rather than the seven-instruction static
body answer, proving backward branches count each executed iteration.

`test_cdc_emulated_time` drives the shipping CDC through the same nominal sector deadline twice:
the instruction-heavy case distinguishes deadline-minus-one from due, while the yield-heavy case
executes no guest instructions and becomes due after the shared NTSC field boundary. Its mixed case
proves instruction work is not added atop the field interval, and its two half-field case retains the
fractional duration. `test_pace_plan` proves `PSXPORT_NOPACE` suppresses host sleeping without losing
the normalized guest field cadence.

Vagrant Story's live first ReadN armed at guest tick 83,098,580 with deadline 83,324,372 and serviced
at 83,324,373: the nominal double-speed 225,792-tick threshold plus one batched-instruction overshoot.
Every following event used the same instruction-time delta. Crash Bash independently armed at 1,331,915,
returned from ReadN before the first event, then serviced LBA35799..35987 at 225,792-tick deadlines.
On the real Mega Man X4 `SLUS_005.61` disc, the loader entered `0x80012E38` at field 5, returned to
reach `0x8001512C` and the Capcom-logo functions at field 67, then reached task `0x8001DAF8` at field
83. The old instruction-only clock hit the retail 601-iteration timeout after LBA224 and rearmed the
request; the shared clock serviced LBA225 and continued through LBA269. Neither execution path uses
a CDC worker, a host-time deadline, or a title-specific drive rate.

## What would falsify it

Pausing under GDB, descheduling the process, or changing `PSXPORT_NOPACE` changes first-INT1 ordering
for the same guest instruction/display sequence; a yielded display field cannot reach a deadline
that elapsed during the wait; interpreted and emitted copies of the same window report different
instruction ticks; instructions already spent inside a field are counted again on delivery; or any
CDC sector event is armed from host wall time. Claiming physical drive-rate or cycle accuracy before
the remaining part of issue 0007 is resolved also falsifies the scope of this claim.

## Re-confirmed 2026-08-22

The focused emulated-time, continuous-read, drive-rate, pace-plan, and interpreter parity gates pass.
The real Mega Man X4 loader crosses all six traced front-end boundaries through `0x8001DAF8` under
`PSXPORT_NOPACE=1`; display delivery, not host sleeping, supplies the missing elapsed guest time.

## Re-confirmed 2026-08-22

Post-composition Clang CTest 90/90 passed emulated-time, continuous-read, pace-plan, interpreter, and emitter controls.
