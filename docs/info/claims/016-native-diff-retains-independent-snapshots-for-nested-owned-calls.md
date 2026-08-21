---
id: C016
kind: claim
status: holds
created: 2026-08-21
tags: ndiff,ownership,nesting,snapshot
depends: runtime/recomp/native_diff.cpp#ndiff_run, tests/test_native_diff.cpp#test_equivalent_nested_calls_keep_independent_snapshots, tests/test_native_diff.cpp#test_mutated_nested_child_reports_the_other_answer
reconfirmed: 2026-08-21
verified_at: 2026-08-21 13:58:09
---

## Claim

Every active `ndiff_run` retains an independent complete guest-state snapshot, so a separately owned
child cannot replace its parent's rewind state, captured native answer, or final native result.

## Evidence

`test_native_diff` drives the shipping API with an owned parent whose native and substrate legs each
invoke the same nested owned child. On the singleton-buffer implementation, the supposedly equivalent
parent reports four fabricated RAM-byte differences and returns the wrong outer memory result. With one
reusable `SnapshotFrame` per active depth, both child calls and the parent match, zero divergences are
recorded, and both child and parent output words retain their exact native values.

The same test independently mutates the nested child's native word. Both child invocations report four
real byte differences, the divergence count rises by exactly two, and the equivalent parent still
matches while retaining the mutated native result. This is the required other answer through the same
production path, not a duplicate comparison helper.

## What would falsify it

An equivalent nested parent/child reports any divergence; an outer comparison begins from a child's
pre-state or leaves a child's partial result in place; nested depth causes a crash or stale reference;
the independently mutated child is not reported twice; or a real consumer still reports a parent-only
saved-stack-slot mismatch after rebuilding against this implementation.

## Re-confirmed 2026-08-21

Post-integration shipping native-diff nested snapshot gate passed both equivalent and mutated cases within the full Clang CTest 81/81 suite.
