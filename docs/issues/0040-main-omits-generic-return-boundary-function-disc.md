---
id: 40
title: MAIN omits generic return-boundary function discovery
status: resolved
symptom: Runtime-patched MAIN callbacks repeatedly fail with recomp-MISS and require manual address seeds even though equivalent overlay handlers are discovered automatically
tags: recompiler,automation,seeds,main,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

`func_entries_after_return()` and `emit_module()` already formed one generic, mergeable function-
boundary pipeline, but only overlay emission passed the candidates into it. The top-level MAIN path
called `emit_module()` without `soft_seeds`. Runtime-patched vtable/callback slots are zero in the
retail file, so pointer scans cannot name their values; ordinary MAIN handlers immediately following
another function's `jr ra` were therefore omitted until a live selector produced a fatal miss.

Crash Bash exposed the cost at the computed method call `0x80012420`: four selector values became four
separate miss/add-seed/rebuild cycles even though the existing boundary scan recognizes all four.

## What was tried / dead ends

Replacing native frame/VSync ownership with whole-program guest suspension was considered and rejected.
It contradicts the product architecture and addresses the wrong cause: the finite native frame driver
is small, while repeated indirect-entry discovery is the manual work that scales with execution depth.

Blindly preserving the observed addresses as explicit seeds was also rejected. It leaves two authorities
for evidence already present in the binary and guarantees the same cycle for the next title.

## Resolution

MAIN now supplies `func_entries_after_return()` candidates to the same `emit_module(...,
soft_seeds=...)` path overlays use. They remain soft: recovered switch cases are pruned, false
early-return boundaries merge back into their containing body, and independent prologue-bearing
functions remain callable. Native dispatch still fails fast for targets that no analysis or explicit
measured residual owns.

The shipping-CLI regression was red on the old path because two unreferenced MAIN handlers were absent
from `shard_disp.c`; it is green after the shared wiring. The full emitter suite passes 66/66.

On verified Crash Bash retail bytes, old emission produced 2,059 functions and 26,331,230 bytes of C.
The shared scan produces 2,450 functions and 26,477,927 bytes: +391 functions but only +146,697 bytes
(0.56%), below the unchanged size guard. Removing five now-redundant MAIN seeds leaves all five targets
dispatchable and preserves the exact 2,450-function/output hash. The rebuilt Clang product reaches two
completed module loads and nested MENU `0x800B5244` with zero recompilation misses, fatal errors, guest
VSync timeouts, or watchdog stalls.

### Resolution (2026-08-30)
MAIN now passes return-boundary candidates through the existing soft-seed merge/prune pipeline; the shipping CLI regression, 66/66 emitter suite, unchanged size guard, and real Crash Bash 2,450-function product boundary all pass.
