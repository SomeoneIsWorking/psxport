---
id: C037
kind: claim
status: holds
created: 2026-08-24
tags: cdc,command,guest-time
depends: runtime/psx/cdc_command_phase.cpp#cdc_command_service, runtime/psx/cdc_native.cpp#cdc_drive_service, tests/test_cdc_command_phases.cpp
---

## Claim

CDC command responses and side effects follow the oracle-derived guest-time phase boundary

## Evidence

test_cdc_command_phases passes 8/8 cases and 48 checks through the shipping CDC path; its pre-deadline cases distinguish the rejected eager answer, and full Clang CTest passes 97/97. An existing Crash Bash one-shot trace at `scratch/logs/crashbash-cdc-phases-once.log` was audited separately: one GetTN returns 02/01/01, the old 0x8002DE2C empty-poll boundary is absent, and guest execution advances through Setloc, Setmode, ReadN, and Pause into later continuous reads.

## What would falsify it

A command response or side effect becomes observable before its computed execution deadline; an argument is consumed outside its 1,815-tick transfer phase; a second-phase completion becomes current before the first IRQ is acknowledged; equal drive/command deadlines expose INT3 before INT1; or a real consumer returns to the former repeated empty-poll boundary after the delayed response.
