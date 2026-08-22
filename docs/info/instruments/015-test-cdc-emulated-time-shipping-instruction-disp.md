---
id: I015
kind: instrument
status: trusted
created: 2026-08-22
---

## Instrument

test_cdc_emulated_time shipping instruction/display guest-time gate

## Validated by

Shipping CDC stays armed at deadline-minus-one and fires exactly at the instruction-heavy deadline; with zero executed instructions it stays armed before a display delivery and fires after one NTSC field. Mixed instruction/field, two-half-field, late-resync, and invalid-cadence cases force distinct answers rather than accepting a uniform clock.

## Known failure modes

The hermetic suite calls `Timing::advanceDisplayFields` directly; it does not prove that every game
routes each real VSync through the shared pacer. The Mega Man X4 real-disc gate supplies that consumer
evidence. It also does not establish cycle-accurate opcode, memory, GTE, DMA, or bus costs; issue 0007
keeps that limitation open.
