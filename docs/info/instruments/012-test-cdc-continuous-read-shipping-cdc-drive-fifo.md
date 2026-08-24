---
id: I012
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

test_cdc_continuous_read shipping CDC drive/FIFO independence test

## Validated by

The instrument has produced both wrong answers. On the consumption-owned model, partial-FIFO and
full-drain cases saw no following INT1. On the immediate BFRD-owned model, an elapsed-zero assertion
produced INT1 before guest code could return. The current suite passes 5/5 and 64 checks through the
shipping controller: no event at deadline-1, event at the deadline, preserved partial cursor,
cancelled Pause deadline, full-drain behavior, nominal 451,584/225,792-tick thresholds, INT1 status 0x22 and
Pause completion status 0x02.

## Known failure modes

This test injects the clock and disc backend, so it proves controller semantics but not that a game
actually advances the production clock. `test_interp_guest_cycles`, emitter execution tests, and
live cdcpace consumer traces cover that separate wiring.
