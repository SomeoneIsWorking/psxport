---
id: 24
title: Native syscall HLE discarded the guest exception record
status: resolved
symptom: Crash crossed EnterCriticalSection but the shipping Core lacked the independent CPU Cause=0x20, exact EPC, and RFE-equivalent Status transition, preventing an honest post-syscall differential.
tags: runtime,syscall,exception,r3000,cp0,crash,oracle
created: 2026-08-25
updated: 2026-08-25
---

## Root cause

`rec_syscall` changed the HLE interrupt flag directly and returned. It did not model R3000A syscall exception entry/return, and generated syscall sites did not supply their exact instruction PC. The diagnostic observer also covered only emitted instruction sites, so an external BIOS target such as B0:56 could execute before comparison.

## Resolution

The emitter and interpreter pass the exact syscall PC to one `syscall_exception` owner. It records EPC and Cause code 8, pushes/pops the low Status mode stack, and applies the requested return interrupt state. Non-generated dispatch misses are observed immediately before BIOS/HLE mutation; generated entries remain owned by emitted checkpoints, avoiding double counts. The independent oracle has a checked syscall-return seam with explicit selector/v0 and CP0 evidence.

## Evidence

Crash direct differential agrees 34/34 at the first post-syscall boundary: PC 0x000000B0, t1=0x56, ra=0x800431B8. Both legs report Cause 0x20, EPC 0x8003E1FC, and resume 0x8003E200. Focused runtime/observer/oracle tests pass, the emitter suite passes 42/42, and a wrong syscall selector is refused without post-return evidence.
