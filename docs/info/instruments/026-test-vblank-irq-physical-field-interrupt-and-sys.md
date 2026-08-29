---
id: I026
kind: instrument
status: trusted
created: 2026-08-30
---

## Instrument

test_vblank_irq physical-field interrupt and SysEnq chain gate

## Validated by

Whole fields latch and dispatch; a refused advance and the first fps60 half-field produce no VBlank, while masked delivery stays pending and dispatches exactly once after unmask.

## Known failure modes

(none recorded yet)
