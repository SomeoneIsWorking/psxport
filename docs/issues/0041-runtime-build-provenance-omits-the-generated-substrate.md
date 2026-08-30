---
id: 41
title: Runtime build provenance omits the generated substrate actually compiled
status: resolved
symptom: A consumer log identifies app and framework commits but cannot prove which ignored generated dispatch/function set the running binary contains.
tags: recomp,provenance,generated,diagnostics,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

`generated/` is deliberately ignored and rebuilt from restricted inputs. The runtime build stamp used
only app and framework Git descriptions, while diagnostics compared an older binary with whatever
generated sources happened to be present later. Crash Bash issue 0018 exposed the failure mode: an
ordinary `0x80012840` discovery miss in pre-seed build `d2c4465` was called an impossible flaky router
miss after later build `9edf471` added the target and regenerated a switch containing it.

## Resolution

The emitter hashes the exact emitted translation units plus MAIN/overlay routing metadata, prefixes the
hash with `RECOMP_VERSION`, and writes the result to `.recomp_identity` and compiled symbol
`g_rec_substrate_id`. `RecompRegistry` carries the value across the consumer/framework seam and
announces it unconditionally; a pre-identity consumer is explicitly `UNKNOWN`.

The shipping CLI regression emits two different bodies and requires different 64-hex identities while
binding each stamp to its compiled table. The runtime seam regression proves both the known and UNKNOWN
installation answers. The unconditional dispatch-decision ring created for the falsified Crash Bash
race was removed rather than retained as permanent hot-path overhead.
