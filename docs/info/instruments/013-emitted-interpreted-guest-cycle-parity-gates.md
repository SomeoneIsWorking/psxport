---
id: I013
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

Emitted/interpreted guest-instruction-time parity gates: `tools/recomp/test_emit.py` and
`tests/test_interp_guest_cycles.cpp`.

## Validated by

Both shipping execution engines run the same addiu/addiu/jr/nop window and report four instruction
ticks. The emitter test also runs a five-iteration backward loop: the correct executed answer is 23 ticks;
the known-wrong static-body answer is seven. The interpreter integration case sets a real CDC event
deadline at cycle four and proves the shipping timing/IRQ wiring consumes it.

## Known failure modes

The compact differential covers integer/control instruction timing and the CDC wiring, not
instruction-specific PSX wait states. The current drive model deliberately uses executed CPU-cycle
units; if memory/coprocessor latency becomes modeled, this instrument must gain those unequal-cost
answers before claims rely on them.
