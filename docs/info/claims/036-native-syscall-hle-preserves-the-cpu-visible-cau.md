---
id: C036
kind: claim
status: holds
created: 2026-08-25
tags: syscall,cp0,oracle
depends: runtime/recomp/syscall_exception.cpp#enter, runtime/recomp/hle.cpp#rec_dispatch_miss, tools/oracle/oracle_shim.c#oracle_resume_syscall_return
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:53:01
---

## Claim

Native syscall HLE preserves the CPU-visible Cause, EPC, Status-stack return, and exact first external target boundary

## Evidence

Crash direct differential agrees 34/34 at PC 0x000000B0 with t1=0x56 and ra=0x800431B8 after both legs record Cause=0x20, EPC=0x8003E1FC, and resume=0x8003E200. test_syscall_exception, test_dispatch_observer, oracle_trace_selftest, and the 42/42 emitter suite pass; wrong selector and generated-entry double observation are explicit negatives.

## What would falsify it

if the independent CPU and shipping Core disagree on Cause/EPC/Status or the first pre-HLE target after the same syscall, or a generated entry is observed twice

## Re-confirmed 2026-08-25

Crash direct differential agrees 34/34 at B0:56 after modeled Cause/EPC/resume; current Clang syscall, dispatch-observer, oracle-trace, and 42/42 emitter gates all pass, including generated-target deduplication and wrong-selector negatives.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 1e3afdfb: test_syscall_exception, test_dispatch_observer, oracle_spike, and oracle_trace_selftest passed in full 99/99.
