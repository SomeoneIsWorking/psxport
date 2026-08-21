---
id: 10
title: Nested native differential overwrites the parent snapshot
status: resolved
symptom: Spyro's byte-faithful InitSpuHardware owner reports four RAM differences at its nested WriteSpuRamPio saved-return-address slot even though both child calls match.
tags: ndiff,ownership,nesting,snapshot,spyro
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

`ndiff_run` already tracked nested execution depth so asynchronous host work remained deferred until
the outer comparison completed, but all active comparisons shared one set of RAM, scratchpad, and GTE
snapshot buffers in process-global `State`. A child comparison overwrote the parent's pre-state and
captured native result. The outer substrate leg therefore started from the child's entry RAM, and the
outer comparison read the child's result as the parent's native answer.

Spyro exposed the exact ownership relationship. Its native `InitSpuHardware` parent calls the separately
owned `WriteSpuRamPio` child. The only reported bytes were `94 BD 05 80` at
`0x801FFEF8..0x801FFEFB`: little-endian `0x8005BD94`, the executable-derived return address that the
parent sets and the child saves at `[sp+40]`. This was snapshot corruption, not a guessed game-body
diagnosis.

## Resolution

The shipping implementation now retains one reusable `SnapshotFrame` per active nesting depth. Each
frame owns its complete pre/native/substrate RAM, scratchpad, and GTE states; register captures remain
call-local. The frame container keeps outer references stable while a deeper frame is first allocated,
and completed depths reuse their vector capacity on later calls.

`test_native_diff` goes through `ndiff_run` itself. Before the fix, its equivalent nested parent failed
with four fabricated RAM differences and the outer call left the wrong memory result. With the fix, the
equivalent child calls and parent all match and the native result remains intact. A second case mutates
only the nested child's value: both child invocations report the expected four byte differences while
the parent still matches, proving that independent frames did not silence nested diagnostics.

The real Spyro parent/child gate must be repeated after this framework change is landed and pinned; that
consumer result is intentionally not inferred from the hermetic test.
