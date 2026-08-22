---
id: I017
kind: instrument
status: trusted
created: 2026-08-22
---

## Instrument

oracle_spike same-CPU IRQ/unsupported-device discriminator

## Validated by

RED before irq.c integration: the 43-check plan ran but the IRQ fixture stopped on WRITE16 0x1F801074, read back zero, and never executed its caller continuation (3 failures). GREEN after routing only I_STAT/I_MASK through vendored Mednafen irq.c: 43/43 passed, while GPUSTAT 0x1F801814 still produced the opposite unsupported-hardware stop; full Clang CTest passed 85/85.

## Known failure modes

(none recorded yet)
