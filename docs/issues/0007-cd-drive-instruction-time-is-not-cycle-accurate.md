---
id: 7
title: CD drive instruction-time is deterministic but not cycle-accurate
status: open
symptom: VSync-yielding titles can take hundreds of fields to reach a ReadN deadline; ordering is stable, but nominal 75/150-sector thresholds still do not prove physical drive-rate accuracy
tags: cdrom,timing,r3000,recompiler,interpreter,accuracy
created: 2026-08-21
updated: 2026-08-22
---

## Resolved: yielded-field starvation

The CDC tick callback read `Timing::guestInstructionTicks`, while a title such as Mega Man X4
executes a small amount of guest code and yields at every VSync. A 2x sector deadline is 225,792
nominal CPU ticks, so counting only the instructions between yields delayed a sector for roughly 250
display fields. Host sleeps and a title-specific faster drive would only mask that ownership defect.

`EmulatedTime` is now the one per-Game CPU-time owner. Emitted/interpreted instructions advance it,
and the shared display-field boundary advances it to the next boundary derived from the guest's GP1
NTSC/PAL standard, even when `PSXPORT_NOPACE` disables host sleeping. Q32 accumulation retains the
fractional NTSC duration; instruction work already spent inside a field is not added twice, and a
late CPU resynchronizes the following boundary without catch-up debt.

The hermetic `test_cdc_emulated_time` forces the shipping CDC deadline through instruction-heavy and
yield-heavy paths, distinguishes deadline-minus-one from due, checks two half-fields against one
whole field, and rejects invalid cadence. On the real `SLUS_005.61` disc, the former loader boundary
at `0x80012E38` returned at field 67, reached `0x8001512C`, and continued through the Capcom-logo task
`0x8001DAF8` at field 83. The old clock hit the retail 601-poll timeout after LBA224 and rearmed the
request; the shared clock serviced LBA225 and continued through LBA269.

## Remaining proven limitation

Executed code currently advances emulated time by one tick for an ordinary emitted/interpreted
instruction and two ticks for a control instruction plus its delay slot. This makes sector ordering
deterministic and gives emitted, interpreted, and yielded-field execution one shared time domain, but
it does not model R3000
opcode latency, cache effects, memory wait states, GTE occupancy/stalls, or DMA/bus contention.

The 451,584 and 225,792 deadline thresholds come from 33,868,800 divided by the nominal 75/150
sectors per second. Applying those physical-cycle thresholds to one-tick-per-instruction time is an
ordering approximation. Cross-game runs prove that it removes the synchronous FIFO race; they do
not prove exact wall-clock-equivalent drive rate.

## Proper next step

Extend the authoritative emulated CPU timing owner with measured opcode, memory and coprocessor
stall costs. A differential instrument must
compare timestamped sector delivery against the true oracle across code paths with materially
different instruction and stall mixes. Until that passes, diagnostics and claims must say
"emulated CPU ticks" or "nominal threshold", never "cycle-accurate" or "physical 75/150 Hz".
