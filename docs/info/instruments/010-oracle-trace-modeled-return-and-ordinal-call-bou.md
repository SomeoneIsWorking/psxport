---
id: I010
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

oracle_trace modeled-return and ordinal-call boundary capture

## Validated by

Permanent 8/8 both-answer suite: ordinal 1 and 2 must produce different targets; the compatibility alias must equal ordinal 1; a capture must contain 33 register records; ordinal 3 must refuse with 2-of-3 and no boundary; correct A(39h) must resume to a subsequent call; wrong A(38h) must refuse with no modeled/post evidence. oracle_spike separately proves the generic resume API accepts an exact settled boundary and refuses wrong target, wrong return-PC, and pending-load states without mutation.

## Known failure modes

(none recorded yet)
