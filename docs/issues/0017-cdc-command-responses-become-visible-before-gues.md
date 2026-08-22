---
id: 17
title: CDC command responses become visible before guest-time completion
status: open
symptom: Crash Bash GetTN 0x13 queues INT3 synchronously; rec_irq_poll drains and ACKs 02/01/01 before caller 0x8002DE2C reaches its bank-1 IRQ flag E0 poll, which then loops forever.
tags: cdc,timing,guest-time,gettn,crashbash
created: 2026-08-22
updated: 2026-08-22
---

Future generic capability. Root cause boundary: command register write currently makes a response synchronously available instead of scheduling command-response availability in guest time. Evidence: crashbash/scratch/logs/crashbash-post-menu-cdregs.log around line 18436 with the consumer's exact stack. Proper fix is scheduled availability shared by all commands/consumers; do not add a Crash Bash HLE, watchdog, retry, or polling special case.
