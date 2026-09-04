---
id: 17
title: CDC command responses become visible before guest-time completion
status: resolved
symptom: Crash Bash GetTN 0x13 queues INT3 synchronously; rec_irq_poll drains and ACKs 02/01/01 before caller 0x8002DE2C reaches its bank-1 IRQ flag E0 poll, which then loops forever.
tags: cdc,timing,guest-time,gettn,crashbash
created: 2026-08-22
updated: 2026-08-24
---

Root cause boundary: command-register writes made responses synchronously available instead of
scheduling command receive and execution in guest time. Evidence: the Crash Bash consumer trace at
`scratch/logs/crashbash-post-menu-cdregs.log` around line 18436 captured the exact call stack. The
fix belongs to the shared controller; a Crash Bash HLE, watchdog, retry, or polling special case
would only conceal the ordering defect.

### Resolution (2026-08-24)
Resolved generically in the shipping CDC path: command writes now arm the oracle-derived receive/argument/execution phase machine instead of running effects synchronously. GetTN INT3 appears at the deterministic 20,815-tick floor; parameter commands add 1,815 ticks per argument; side effects occur only at execution; multi-phase INT2 waits for the prior IRQ to be acknowledged; drive events win exact deadline ties. test_cdc_command_phases forces early/due, argument transfer, delayed effects, invalid argument count, replacement, IRQ separation, and tie-order answers; full Clang CTest passed 97/97 on 2026-08-24.

The pre-landing Crash Bash one-shot log `scratch/logs/crashbash-cdc-phases-once.log` supplies the real-consumer discriminator: GetTN occurs exactly once and returns 02/01/01, the former `0x8002DE2C` empty-poll PC occurs zero times, and the guest proceeds through 6 Setloc, 1 Setmode, 6 ReadN, and 5 Pause commands. It reads continuous ranges 35799..35987 and 17558..17655 without an executor fault, CD timeout, unhandled command, fatal, or positive controller-zero fill. The bounded log ends during an active read, so it proves this command handoff rather than a first frame or gameplay.
