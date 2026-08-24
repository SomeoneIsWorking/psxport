---
id: I010
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

oracle_trace modeled-return, ordinal-call, and exact-PC boundary capture

## Validated by

Permanent 12/12 both-answer suite: ordinal 1 and 2 must produce different targets; the compatibility
alias must equal ordinal 1; a capture must contain 33 register records; ordinal 3 must refuse with
2-of-3 and no boundary; correct A(39h) must resume to a subsequent call; wrong A(38h) must refuse with
no modeled/post evidence. Exact-PC capture must reach an indirect `jalr` target before its first
instruction with 33 register records, refuse an unreachable target with its executed denominator, and
refuse a successor reached only after an unsupported hardware access. `oracle_spike` separately proves
the generic resume API accepts an exact settled boundary and refuses wrong target, wrong return-PC, and
pending-load states without mutation.

## Known failure modes

An emulator device callback can advance BACKED_PC before returning a hardware stop. That successor is
tainted and must not be emitted as a PC boundary; `--capture-at` accepts only an initial state or a
clean `ORACLE_STOP_BUDGET` successor. A requested PC outside the executed window is a refusal, not an
empty successful capture.
