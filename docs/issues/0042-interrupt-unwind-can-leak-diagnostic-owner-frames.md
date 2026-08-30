---
id: 42
title: Interrupt unwind can leak diagnostic owner frames
status: open
symptom: A HookEntryInt custom exception exit unwinds through generated wrappers without restoring their diagnostic attribution depth.
tags: irq,diagnostics,attribution,unwind,recompiler
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

Generated dispatch wrappers push an `otattr` owner before calling a body and pop it on ordinary C++
return. HookEntryInt's custom exception exit throws `ReturnFromException` through that wrapper, so the
manual pop is skipped. `Hle::irqPoll()` snapshots and restores the complete guest `R3000`, but it does
not restore `InterpDiag::otattr_depth`. The leaked frame cannot change dispatch routing, but later
diagnostics can report the wrong current owner and eventually exhaust the bounded attribution stack.

## Required fix

Snapshot the attribution depth at the interrupt boundary and restore it on every exit, including the
custom unwind. Add a hermetic regression that starts with a synthetic outer owner, dispatches a handler
whose custom exit throws through a wrapper, and proves both the original depth/top and the complete
guest register file are restored after `irqPoll()`. The test must exercise the shipping IRQ and wrapper
ownership seams rather than duplicating their state transitions.
