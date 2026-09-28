---
id: 38
title: A budget resume that STARTS on a host-service leaf completes it with a STALE $ra
status: open
symptom: A guest task's continuation becomes a non-code address and execution faults with "ambiguous code-image identity"
state_items: MMX4-S002
tags: psxport,native-dispatch,resume,root-cause
created: 2026-09-29
updated: 2026-09-29
---

## The root cause, read from the shipping code

`NativeExecutionScope` (`runtime/cpu/native_dispatch.cpp`) captures its continuation at scope entry:

    continuation_(core.r[31])

and its own comment states the assumption plainly:

> WHERE THE GUEST CONTINUES after this leaf, which is `r[31]` — the address the guest's own `jal` left
> there.

`invokeNativeFunction` then ends a leaf with `execution.completeReturn()`, which sets
`core_.pc = continuation_`, and the caller's `requested->guestPc` takes that same value as the exit's
guest PC.

**That assumption holds for a `jal` and is violated by a resume.** The resume path is:

    resumeGuestToReturnFrom(core, entry, resumePc, returnPc, budget)
      -> executeWithBoundary(guestAddress = resumePc, returnAddress = returnPc, dispatchHostServices = true, ...)
        -> nextPc = lightrec_execute(state, nextPc, ...)      // execution STARTS at resumePc
          -> dispatchGuestHostService(core, resumePc)          // the leaf is entered, not called
            -> invokeNativeFunction -> NativeExecutionScope    // captures core.r[31] HERE

**No `jal` executed, so `r[31]` is whatever the guest last left there** — a stale value from some
earlier point in the same task, not a return address for this call. The leaf completes, and the
continuation is that stale value.

## Why this explains a rare fault inside a common event

Measured on Mega Man X4 (`megamanx4/docs/issues/0037`), classifying all **1,561** budget resumes by
where the continuation lands:

| continuation lands in | count | share |
|---|---|---|
| BIOS range `0x800E0000..0x80100000` | 1,261 | 80.8% |
| game text `0x80010000..0x8012F800` | 300 | 19.2% |
| anywhere else | 0 | 0% |

**Four in five resumes start execution on a BIOS address, which for this title is an HLE entry and not
a code address** — so four in five resumes enter a host-service leaf through the resume path rather
than through a `jal`, and take a stale `$ra` as their continuation.

**Whether the run survives therefore depends on whether the stale value happens to be a valid code
address**, which is exactly the observed shape: `0x800ED744` is a known entry and 583 resumes through
it succeed; the faulting run is the one where the stale `$ra` was `0x0113D7D0`, a RAM-range address in
no active code image, which `resolveHostDispatch` correctly reports as
`Fault` / "ambiguous code-image identity".

**This also explains why the value is never at rest.** It is a register, briefly, in a leaf that has
already returned. It is not a stored constant, not a table entry, and not a stack word — which is why
0 of 43,520 word-reads over four RAM regions found it, and why the 0-of-294,912 static image scan
found nothing.

## Why the port's own guard did not catch it

`megamanx4/game/core/bios_threads.cpp` does `resumeAddress = result.guestPc` and guards it with
`result.cycles == 0u || result.guestPc == 0u` — a **non-zero** check. The code already knows a bad
continuation must be stopped for exactly this reason (it aborts on a zero PC) and stops one predicate
short. That guard is in the title; **the defect is upstream of it**, in the continuation the framework
hands it.

## The fix, and why it is not applied yet

**Direction.** A leaf entered by a resume has no `jal`, so its continuation cannot come from `r[31]`.
The resume already carries the correct value: `returnPc`, the activation's return address, passed to
`executeWithBoundary` and currently used only as the return boundary. The fix is to let the boundary
supply the continuation when the leaf was entered by a resume rather than by a call.

**Why it is not applied yet.** This is shared framework code on every title's hot path, and the change
touches the continuation of every host-service leaf. Applying it without a measurement that *shows* the
stale `$ra` would be fixing a code reading rather than an observed cause, and the blast radius is
every port. The next step is the measurement, not the patch.

## The next step, named

1. **Measure it before changing it.** Report `r[31]` at host-service leaf entry **when the leaf was
   entered by a resume rather than by a `jal`**, with a count and the share that are stale. The
   predicted figure is the **1,261 of 1,561 (80.8%)** resume-entered BIOS-range leaves measured on
   MMX4; if the stale-`$ra` rate comes back far below that, this root cause is wrong.
2. **Distinguish stale from merely old.** A stale `$ra` that is still a valid text address is harmless
   today and is the majority case. The report needs both numbers, or "harmless" will be read as
   "correct".
3. **Only then patch**, with a regression test that resumes a host-service leaf with a poisoned `r[31]`
   and asserts the continuation is the resume's `returnPc`. The test must fail on the current code, or
   it is not testing the defect.
4. **Then, and only then, revisit the title's guard.** `guestPc == 0` is still too weak, but tightening
   a symptom while the framework hands it a bad continuation is treating the messenger as the problem.

## Falsifier

* If a report of `r[31]` at resume-entered host-service leaves shows it is consistently a valid text
  address, the stale-`$ra` mechanism is not producing the fault, and the root cause is elsewhere.
* If the faulting task's `r[31]` is a valid code address at the moment the leaf completes, this
  account is wrong regardless of the aggregate rate.
* If a port with **no** BIOS-range resumes never shows the fault while one with them always does, that
  is corroboration; the converse would refute it.
