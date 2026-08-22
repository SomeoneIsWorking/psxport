---
id: 18
title: Recompiler loses internal jr jump-table reentry labels
status: resolved
symptom: Toy Story 2 FUN_800100E4 reenters its internal 32-way jr table at 0x8001040C, loops, then misses 0x800104E4 because generated C emits sequential unlabeled gotos for 0x800103EC..0x800104E4.
tags: recompiler,jump-table,jr,reentry,toystory2
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

`_scan_computed_offset` recognized an immediate table base formed by `lui` plus `addiu`, but not the
equivalent `lui` plus `ori` sequence in Toy Story 2. It therefore failed to recover the resident
renderer’s `jr (base + ((index & 31) << 3))` control flow and emitted an external dispatch for a
target that is a local `j`/`nop` trampoline.

Accepting `ori` alone exposed a second generic defect: the fallback scanner kept collecting
jump-shaped blocks after the table. The decoded `andi index, source, 31` is an exact 32-slot bound;
ignoring it merged adjacent shared case bodies into the table.

Cross-consumer verification then exposed a third defect in the same recovery pass. Its forward
constant map did not invalidate a register after an intervening load and treated a bare `lui` as a
complete target. In Vagrant Story this combined unrelated constants from a duplicated tail into a
false unaligned local target and eventually walked beyond the executable image.

## Resolution

The emitter now folds both standard low-half forms through one constant-construction helper. For a
left-shifted index whose nearest definition is a contiguous low-bit `andi` mask, it derives the exact
case count from that mask and returns only those fixed-stride targets. Unbounded legacy shapes retain
the existing conservative scan. There is no title, address, seed-list, or generated-code special
case. Reaching constants are now invalidated by every register definition, only completed low-half
assignments count as targets, and all recovered instruction targets must be word-aligned.

The hermetic four-way `lui`/`ori` trampoline test failed before the change by dispatching
`0x80010020` and returning zero instead of 11. It now executes all four cases, requires exactly the
four slot labels, and explicitly rejects the adjacent shared body as a slot. The focused emitter and
decoder suite passes 55 tests; the decoder suite is now a normal CTest target, and the full Clang
framework build and 85-test CTest suite pass.

Toy Story 2 was regenerated without the diagnostic `0x8001040C` product seed. Its generated resident
function contains exactly the 32 slots `0x800103EC..0x800104E4`; a bounded real-disc run reaches both
the observed `0x8001040C` slot and the negative sibling `0x800104E4` with no recompilation miss. It
advances to the later independent RenderQueue-capacity boundary.

Vagrant Story’s no-argument path also completes fresh resident/TITLE emission under the corrected
recompiler (799 resident functions and 137 TITLE functions), proving the prior out-of-image failure
is rejected rather than hidden behind cached generated output.

### Resolution (2026-08-22)

The generic `lui`/`ori` folding and exact mask-derived slot bound are verified by controlled
synthetic execution, full framework tests, exact generated-slot census, and the bounded real-disc
consumer continuation. Falsifiers are an extra or missing recovered slot, either measured Toy Story
slot reaching external dispatch, a non-contiguous mask being treated as a case count, or a previously
supported unbounded computed-offset idiom losing its targets.
