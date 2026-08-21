---
id: I016
kind: instrument
status: trusted
created: 2026-08-21
tags: ndiff,ownership,nesting,snapshot,hermetic
---

## Instrument

`tests/test_native_diff.cpp` — shipping `ndiff_run` nested-snapshot gate.

## Validated by

The positive case runs one equivalent parent comparison containing two equivalent child comparisons.
The old singleton snapshot storage fails the parent with four fabricated RAM differences and corrupts
its final memory; independent depth frames produce zero divergences and preserve both exact output words.

The opposite case changes only the nested child's native output. The shipping differential reports four
byte differences on each of its two invocations, the global count advances by exactly two, and the parent
remains matched. The test therefore proves both that nesting is isolated and that isolation does not hide
a real child mismatch.

## Limits

The hermetic gate covers RAM, nesting, result restoration, and divergence accounting. `ndiff_run` also
snapshots scratchpad, GPRs, hi/lo, and GTE state through the same per-depth frame/local capture path, but
this focused test does not independently mutate every one of those surfaces. Real consumer gates remain
required for game-specific control flow and host-only state that NDIFF does not snapshot.
