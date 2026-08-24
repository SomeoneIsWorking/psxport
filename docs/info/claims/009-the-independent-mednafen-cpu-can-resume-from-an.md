---
id: C009
kind: claim
status: holds
created: 2026-08-21
tags: oracle,harness,bios
depends: tools/oracle/oracle_shim.c#oracle_resume_call_return, tools/oracle/oracle_trace.c#main, tools/oracle/oracle_spike.c#modeled_call_return_case
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:52:56
---

## Claim

The independent Mednafen CPU can resume from an explicitly modeled external-call return without executing vector memory, while refusing a stale target boundary.

## Evidence

Clang-built oracle_spike planned and passed 39/39 checks. Its modeled-return cases reached 0xA0 through real jal/jr delay slots, made wrong target, wrong return-PC, and pending-load boundaries refuse without changing PC, accepted the exact settled target/ra, preserved an unrelated register and timestamp, installed explicit v0/v1, and executed the caller. oracle_trace's both-answer CLI suite passed 8/8, including accepted A(39h) and refused A(38h) models.

## What would falsify it

A change to oracle_resume_call_return, oracle_trace modeled-return dispatch, CPU boundary state, or the spike/CLI fixtures; falsify if a wrong or unsettled boundary mutates state, vector code executes, timestamp resets, or the resumed caller does not run.

## Re-confirmed 2026-08-25

Reconfirmed by oracle_spike's 17 modeled-call-return checks inside the complete 84/84 run, plus oracle_trace CLI modeled-return positive/wrong-function checks in the 12/12 selftest.

## Re-confirmed 2026-08-25

oracle_trace_selftest passes 14/14 after the same oracle shim change, covering both successful modeled BIOS/syscall returns and wrong-selector refusals.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 1e3afdfb: oracle_trace_selftest passed and exact CPU/device capture remained green in full 99/99 CTest.
