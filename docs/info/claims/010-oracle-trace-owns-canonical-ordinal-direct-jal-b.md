---
id: C010
kind: claim
status: holds
created: 2026-08-21
tags: oracle,harness,trace
depends: tools/oracle/oracle_trace.c#main, tools/oracle/test_oracle_trace.py#main
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:11:05
---

## Claim

oracle_trace owns canonical ordinal direct-jal and exact pre-execution PC boundary capture, and refuses
unreached or hardware-tainted targets with explicit executed denominators.

## Evidence

The Clang-built test_oracle_trace.py suite passed 12/12. `--capture-call 1` captured 0x80010040,
`--capture-call 2` captured the distinct 0x80010050, and `--capture-call 3` exited 2 saying reached 2
of 3 while writing no CALL-BOUNDARY block. `--capture-at` reached a synthetic indirect `jalr` target
after four executed instructions and captured its canonical 33-register pre-execution block. An
unreachable target exited 2 with 8-of-8; a target visible only after an unsupported GPU-register write
exited 2 with 3-of-8 and wrote no PC-BOUNDARY block. The initial-PC case is handled before stepping.

## What would falsify it

A change to oracle_trace call decoding/delay-slot tracking, capture-selector parsing/output, or
test_oracle_trace.py; falsify if two ordinals alias, an insufficient window exits success, a PC target
is captured after its instruction, the register block is not canonical, or a hardware-tainted
successor writes a boundary.

## Re-confirmed 2026-08-25

Reconfirmed after exact-PC capture commit: oracle_trace selftest passes 12/12 across distinct ordinals, alias, canonical register blocks, indirect capture-at, unreachable denominator, modeled-return positive/opposite, and unsupported-predecessor refusal.
