---
id: 0039
title: The store observer invalidates EVERY block and instruments EVERY store, so a run with it armed is not the program
status: open
kind: finding
owner: psxport
---

# The store observer is far more invasive than "watch one PC" suggests

**MEASURED 2026-09-29, from the source and then reproduced end to end.** Found while arming
`PSXPORT_STORE_OBSERVE` on Mega Man X4's guest scheduler, where it changed the program's outcome.

## What arming it actually does — three facts, each read from the implementation

**1. Arming frees EVERY compiled block.** `lightrec_set_store_observer` (`lightrec.c:2019`):

```c
/* A LUT-only invalidation would reuse the old unchanged compiled block. */
lightrec_invalidate_all(state);
lightrec_free_all_blocks(state->block_cache);
```

The comment is the design decision, and it is the correct one — a LUT-only invalidation would reuse
a block compiled without the observer and silently under-report. The cost is that **arming the
instrument discards the entire translation cache**, so every block is re-translated under it.

**2. The emitter then instruments EVERY store in EVERY block, not the watched one.** In
`emitter.c:3056` and `emitter.c:3071` the observer call is emitted at each store, guarded only by
`if (observe_store)`. The PC filter is applied later, in the callback
(`LightrecExecutor::Impl::observeStore` compares `target.guestPc != guestPc`). **So arming one PC
puts a host call on every store the program executes.**

**3. Each observed store flushes and resets the ENTIRE register cache.** `store-observer.c`:

```c
lightrec_clean_regs(reg_cache, _jit);
lightrec_regcache_reset(reg_cache);
```

`regcache` is the optimisation that keeps guest values in JIT registers instead of spilling them to
the CPU context. Resetting it at every observed store means every guest register is spilled and
reloaded around every store in the program.

## Why this matters more than a performance note

**A run with the observer armed is not the program it was measuring.** Any conclusion drawn from
cycle counts, wall-clock, progress-per-frame, or — as in MMX4 — whether a per-call budget is
exhausted, is a conclusion about *the instrumented program*. That is the same class of failure this
project treats as worst: a diagnostic that changes the answer it reports.

It also retroactively bounds every store-observer measurement in this workspace. The counts
themselves are sound — they count real guest stores at the right PCs — but **the run that produced
them is not a clean run**, and no speed, budget or progress claim from such a run is evidence.

## The reproduction, and the part that does NOT add up

MMX4's guest scheduler at `0x80012600` is a polling loop. Two runs, identical flags, one target each,
repeated twice:

| armed store PC | events | outcome |
|---|---|---|
| `0x80012628` (cursor store, function entry) | 400 | **no abort**, 400 frames clean |
| `0x80012724` (cursor advance, inside the loop) | 0 | **aborts** before any event |

**What is NOT established: why the two differ.** By the mechanism above, arming EITHER PC invalidates
every block and instruments every store, so both runs should be equally affected — yet they are
reproducibly different, 2 for 2 each way. I have no verified explanation, and the obvious story
("the loop block is hot, so instrumenting it costs more") does not survive the fact that **both
armings instrument every store everywhere**. Something about which block is entered first, or about
budget accounting, is involved and is not understood.

Reported rather than worked around, per the standing request to send what does not add up.

## The correction this forced on a published result

An earlier MMX4 note (`megamanx4/docs/issues/0037`) resolved a 1-versus-10,403 event-count anomaly
as "run-length variance" and withdrew the worry that arming a PC perturbs execution. **That
resolution was wrong.** It was generalised from two runs of the *same* arming and never compared the
two armings head to head. The head-to-head A/B above shows the effect is real and reproducible. The
worry is reinstated; only the "run-length variance" explanation is withdrawn.

## ADDENDUM — a second, sharper defect: arming a DELAY-SLOT store segfaults

While using the observer to name the exact store that clobbers a guest interrupt element in Mega
Man X4, every run appeared to report *nothing*. **It was not a quiet instrument — those runs were
crashing.** A crashed process prints no teardown report, so "the observer saw no stores" and "the
observer destroyed the process" are indistinguishable in a log, and the first was believed.

Isolated with a control matrix: arming `0x800126A8` — `sh $s1, ($v0)`, the **delay slot of `jal
0x800EDdbc`** — alone exits 139. Arming `0x80012628` alone, four stores in `0x80015F04..0x80015F80`
individually, and two of those together all exit 0. **The discriminating variable is the delay slot,
not the store width and not the count.** Full reproduction in `0040`.

This also corrects the record on the Mega Man X4 side: the silence of the observer on the stores of
the routine at `0x80015ECC` was never evidence about that routine, because those runs died before
printing.
