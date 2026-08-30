---
id: 42
title: Interrupt unwind can leak diagnostic owner frames
status: resolved
symptom: A HookEntryInt custom exception exit unwinds through generated wrappers without restoring their diagnostic attribution depth.
tags: irq,diagnostics,attribution,unwind,recompiler
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

Generated dispatch wrappers pushed an `otattr` owner before calling a body and popped it on ordinary
C++ return. HookEntryInt's custom exception exit throws `ReturnFromException` through that wrapper, so
manual lifetime management skipped the pop. The leaked frame cannot change dispatch routing, but later
diagnostics could report the wrong current owner and eventually exhaust the bounded attribution stack.

## Resolution

Generated wrappers now acquire attribution through `InterpDiag::OtAttrScope`. Normal returns, override
returns, and the private `ReturnFromException` unwind all release the same scope, so the wrapper owns one
balanced lifetime instead of relying on the IRQ boundary to repair leaked state. The generated-substrate
version moved to `2026-08-30.3` because this changes every wrapper body.

The hermetic runtime regression installs the continuation through shipping `B0:0x19`, reaches
`irqPoll -> rec_dispatch -> scoped guest dispatch -> B0:0x17` with two existing outer owners, and proves
all 32 GPRs, `hi`, `lo`, `pc`, attribution depth, top, and caller are restored while the custom
continuation remains non-returning. The emitter regression separately requires every generated wrapper
to use that shipping scope and rejects manual push/pop lifetime management.
