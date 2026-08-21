---
id: C010
kind: claim
status: holds
created: 2026-08-21
tags: oracle,harness,trace
depends: tools/oracle/oracle_trace.c#main, tools/oracle/test_oracle_trace.py#main
---

## Claim

oracle_trace owns canonical ordinal direct-jal boundary capture and refuses an unreached ordinal with an explicit denominator.

## Evidence

The Clang-built test_oracle_trace.py suite passed 8/8. --capture-call 1 captured 0x80010040, --capture-call 2 captured the distinct 0x80010050, and --capture-call 3 exited 2 saying reached 2 of 3 while writing no CALL-BOUNDARY block. The suite also proves --capture-first-call is the ordinal-1 alias and each captured boundary carries the canonical 33-register block.

## What would falsify it

A change to oracle_trace call decoding/delay-slot tracking, ordinal CLI parsing/output, or test_oracle_trace.py; falsify if two ordinals alias, an insufficient window exits success, or it writes a misleading boundary.
