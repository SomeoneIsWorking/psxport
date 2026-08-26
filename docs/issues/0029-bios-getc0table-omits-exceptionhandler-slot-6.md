---
id: 29
title: BIOS GetC0Table omits ExceptionHandler slot 6
status: resolved
symptom: B(56h) returns a private C0 table whose slot 6 is zero, so guests that copy an exception handler through C(06h) dereference the wrong destination
tags: bios,hle,c0,oracle
created: 2026-08-26
updated: 2026-08-26
---

## Root cause

`Hle::workAreaInit` treated its private C0 page as a synthetic callable leaf and initialized only
slots 0/1. B(56h), however, exposes that page as a guest-readable BIOS function table, so leaving
slot 6 zero removed the C(06h) ExceptionHandler destination from the ABI.

## What was tried / dead ends

The shipping dispatch seam proved B56 already returned the intended table base; the missing datum was
the table entry, not syscall continuation or address translation. The independent oracle then carried
the validated selector-1 return through B56 and observed both `0x00000C80` and a deliberate opposite
seed at A44, ruling out a trace that silently ignored modeled RAM.

## Resolution

### Note (2026-08-26)
Crash 1 SCUS_949.00 evidence (consumer issue #7, claim C022, instrument I007, state S003/S004, frontier CRASH1-04): selector-1 syscall Cause=0x20/EPC=0x8003E1FC resumes 0x8003E200; next B(56h) boundary has ra=0x800431B8 and must return 0x8000F800; guest reads slot 6 at 0x8000F818 and copies 14 words into 0x00000C80; next dispatch is A(44h) with ra=0x800431E8. Framework fix remains title-neutral.

### Resolution (2026-08-26)
The shared initializer now preserves slots 0/1, seeds C0 slot 6 with the title-neutral C(06h)
ExceptionHandler address `0x00000C80` through `Core::mem_w32`, and remains one-shot so guest
table/handler rewrites survive. `test_bios_work_area` passes 14/14; ordered `oracle_trace` controls
pass 17/17; the full Clang build and 106/106 CTest including cpp_style/clang-tidy pass.
