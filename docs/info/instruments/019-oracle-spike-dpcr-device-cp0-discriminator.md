---
id: I019
kind: instrument
status: trusted
created: 2026-08-25
---

## Instrument

oracle_spike DPCR/device/CP0 discriminator

## Validated by

Clang-built oracle_spike plans and executes 84/84 checks across ten classes: clean CPU execution, unsupported GPU opposite, IRQ, DPCR reset/full/partial writes with DICR sticky opposite, a real jal device boundary with all-three write provenance, scheduled-event sticky refusal, stepping, RAM mirrors, modeled call return, and CPU-produced syscall CP0 push/refuse/resume. Nonzero SR bits prove push/pop cannot pass by preserving zero.

## Known failure modes

(none recorded yet)
