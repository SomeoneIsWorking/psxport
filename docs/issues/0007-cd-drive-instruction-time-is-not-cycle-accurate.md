---
id: 7
title: CD drive instruction-time is deterministic but not cycle-accurate
status: open
symptom: ReadN ordering is stable across host speed and debugger pauses, but the nominal 75/150-sector thresholds do not prove physical drive-rate accuracy
tags: cdrom,timing,r3000,recompiler,interpreter,accuracy
created: 2026-08-21
updated: 2026-08-21
---

## Proven limitation

`Timing::guestInstructionTicks` currently advances by one tick for an ordinary emitted/interpreted instruction
and two ticks for a control instruction plus its delay slot. This makes sector ordering deterministic
and gives emitted and interpreted execution one shared time domain, but it does not model R3000
opcode latency, cache effects, memory wait states, GTE occupancy/stalls, or DMA/bus contention.

The 451,584 and 225,792 deadline thresholds come from 33,868,800 divided by the nominal 75/150
sectors per second. Applying those physical-cycle thresholds to one-tick-per-instruction time is an
ordering approximation. Cross-game runs prove that it removes the synchronous FIFO race; they do
not prove exact wall-clock-equivalent drive rate.

## Proper next step

Add one authoritative emulated CPU timing owner with measured opcode, memory and coprocessor stall
costs, then make both the static emitter and interpreter consume it. A differential instrument must
compare timestamped sector delivery against the true oracle across code paths with materially
different instruction and stall mixes. Until that passes, diagnostics and claims must say
"instruction ticks" or "nominal threshold", never "cycle-accurate" or "physical 75/150 Hz".
