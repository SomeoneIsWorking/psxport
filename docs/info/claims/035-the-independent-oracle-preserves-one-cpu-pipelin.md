---
id: C035
kind: claim
status: holds
created: 2026-08-25
tags: oracle,dpcr,syscall
depends: tools/oracle/oracle_shim.c#oracle_capture_devices, tools/oracle/oracle_shim.c#oracle_resume_syscall_return, tools/oracle/oracle_spike.c#main, tools/oracle/CMakeLists.txt, vendor/beetle-psx
---

## Claim

The independent oracle preserves one CPU pipeline through I_STAT/I_MASK/DPCR, exposes a clean separately validated device snapshot with actual-write provenance, and resumes a syscall only after validating the CPU-produced CP0 exception state.

## Evidence

Clang oracle_spike 84/84. DPCR reset 07654321; lane-1 AABBCCDD -> BBCCDD00 and lane-2 -> CCDD0000; CTR-shaped jal boundary captured I_STAT=0, I_MASK=0, DPCR=33333333 with valid/writes=ALL; DICR capture and a later step both refused sticky hardware taint, and a scheduled event likewise refused capture plus the next step. A real syscall reached BFC00180 with ExcCode 8, BD 0, EPC at the syscall and SR low bits pushed; wrong selector and BD=1 refused without resume; accepted return installed v0/v1, popped SR once, preserved Cause/EPC, and executed EPC+4.

## What would falsify it

DPCR reset/partial-write behavior, IRQ/DPCR bus routing, sticky taint, device validity/write provenance, CP0 capture, syscall validation/RFE rule, or any oracle_spike expected opposite changes; the 84-check denominator is not reached; or unsupported DICR/event state can emit a device snapshot or resume.
