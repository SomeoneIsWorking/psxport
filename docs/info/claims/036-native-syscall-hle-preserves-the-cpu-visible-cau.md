---
id: C036
kind: claim
status: holds
created: 2026-08-25
tags: syscall,cp0,oracle
depends: runtime/psx/syscall_exception.cpp#enter, runtime/psx/hle.cpp#Hle::dispatchBios, tools/oracle/oracle_shim.c#oracle_resume_syscall_return, tools/oracle/test_oracle_trace.py#main
reconfirmed: 2026-08-26
verified_at: 2026-08-26 23:30:36
---

## Claim

Native syscall HLE preserves the CPU-visible Cause, EPC, Status-stack return, and exact first external target boundary

## Evidence

The independent oracle and shipping syscall owner agree at PC 0x000000B0 with t1=0x56 and
ra=0x800431B8 after recording Cause=0x20, EPC=0x8003E1FC, and resume=0x8003E200.
`test_syscall_exception` and `oracle_trace_selftest` retain wrong-selector and duplicate-boundary
negative controls.

## What would falsify it

The independent CPU and shipping `Core` disagree on Cause/EPC/Status or the first HLE target after
the same syscall, or the boundary is observed twice.

## Re-confirmed 2026-08-25

The syscall and oracle-trace gates pass, including boundary deduplication and wrong-selector
negatives.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 1e3afdfb: test_syscall_exception, test_dispatch_observer, oracle_spike, and oracle_trace_selftest passed in full 99/99.

## Re-confirmed 2026-08-26

Full Clang CTest passes 106/106 after oracle_trace gained ordered selector-1 syscall then BIOS continuation;
its 17/17 selftest retains the standalone syscall answer, refuses the wrong selector, and reaches B56
only after the validated resume. Dependency coverage spans syscall entry, HLE external dispatch, the
oracle resume mechanism, and the ordered CLI control.
