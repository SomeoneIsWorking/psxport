---
id: C042
kind: claim
status: holds
created: 2026-08-26
tags: bios,c0,crash
depends: runtime/psx/hle.cpp#Hle::workAreaInit, tools/oracle/oracle_trace.c#main, tests/test_bios_work_area.cpp#main
reconfirmed: 2026-08-26
verified_at: 2026-08-26 23:30:36
---

## Claim

B(56h) publishes a guest-writable C0 table whose slot 6 is the C(06h) ExceptionHandler address 0x00000C80

## Evidence

Crash 1 reaches B(56h), loads slot 6, copies its 14-word exception handler to that destination, and next reaches A(44h). Clang-focused test_bios_work_area passes 14/14 through Hle::dispatchBios while preserving slots 0/1, adjacent-zero answers, and guest rewrites; oracle_trace_selftest passes 17/17 for selector1 -> B56 -> explicit RAM seed -> A44, alternate seed, and wrong-order refusal.

## What would falsify it

if B(56h) returns a table with slot 6 other than 0x00000C80, a repeat call overwrites a guest rewrite, or the grounded consumer reaches a different table/slot destination

## Re-confirmed 2026-08-26

Full Clang build passed; complete CTest passed 106/106 including `test_bios_work_area` 14/14,
`oracle_trace_selftest` 17/17, cpp_style, and clang-tidy. Slots 0/1 remain covered while slot 6 and
guest persistence carry positive/opposite answers. Dependency coverage spans the shipping work-area
owner, oracle sequencing owner, and both-answer shipping test.
