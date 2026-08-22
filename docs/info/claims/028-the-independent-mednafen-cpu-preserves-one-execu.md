---
id: C028
kind: claim
status: holds
created: 2026-08-22
tags: oracle,irq,harness
depends: tools/oracle/oracle_shim.c, tools/oracle/oracle_spike.c
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:37:04
---

## Claim

The independent Mednafen CPU preserves one execution pipeline through PSX I_STAT/I_MASK bus accesses while unsupported devices remain explicit hardware stops.

## Evidence

oracle_spike planned and passed 43/43 checks: the IRQ fixture wrote/read I_MASK, wrote I_STAT, observed 0x3333 readback, and continued to set t3=0x4444; the GPUSTAT 0x1F801814 fixture still returned ORACLE_STOP_HARDWARE. Full Clang CTest passed 85/85.

## What would falsify it

Falsified if an I_STAT/I_MASK access leaves the CPU, loses the real load delay/timestamp, returns the wrong controller state, or an unsupported device such as GPUSTAT no longer stops.

## Re-confirmed 2026-08-22

Final Clang build and full framework CTest passed 85/85. oracle_spike passed the complete 43/43 same-CPU IRQ and unsupported-GPUSTAT both-answer plan.

## Re-confirmed 2026-08-22

Post-cleanup final gate: oracle_spike passed 43/43 with same-CPU I_MASK/I_STAT continuation and the unsupported GPUSTAT opposite answer; full Clang framework CTest passed 85/85 including cpp_style/clang-tidy.
