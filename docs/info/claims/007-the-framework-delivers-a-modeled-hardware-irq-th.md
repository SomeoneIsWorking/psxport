---
id: C007
kind: claim
status: holds
created: 2026-08-21
tags: bios,irq,hookentryint
depends: runtime/psx/bios_interrupt.cpp#bios_interrupt_dispatch_custom_exit, runtime/psx/hle.cpp#Hle::irqPoll, runtime/cpu/execution_control.cpp#requestExecutionExit
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:52:55
---

## Claim

The framework delivers a modeled hardware IRQ through the measured SysEnqIntRP chain and then the
HookEntryInt saved continuation; B0:0x17 returns a typed execution exit to the interrupted R3000
context rather than continuing in the interrupt handler.

## Evidence

`tests/test_bios_interrupt.cpp` proves restored and refused jmp_buf contexts plus non-return versus
fallthrough. MMX4 `tools/verify_cd_irq.py`, claim C010, and issue 0009 show IRQ2 `0x800E7944`
clearing I_STAT and entering its sync callback. Crash Bash C006 and issue 0004 reach `0x8003F5F0`
and its CD drain. Vagrant `tools/re_vblank.py`, claims 012/014, and issues 0014/0015 retain DICR
`0x00900000`, dispatch DMA4 `0x8001DE94`, and advance VBlank 0 to 179.

## What would falsify it

Any retail-backed consumer whose installed HookEntryInt continuation is skipped, whose source
callback is not reached after its verifier declines, or whose guest execution continues after
B0:0x17 instead of returning the typed boundary.

## Re-confirmed 2026-08-21

Post-string-extraction Clang build and 76/76 CTests passed, including the shipping bios_interrupt suite; Hle::irqPoll behavior is unchanged.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 1e3afdfb: test_bios_interrupt passed in the full 99/99 suite.
