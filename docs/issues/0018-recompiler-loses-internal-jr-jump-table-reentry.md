---
id: 18
title: Recompiler loses internal jr jump-table reentry labels
status: resolved
symptom: Toy Story 2 FUN_800100E4 reenters its internal 32-way jr table at 0x8001040C, loops, then misses 0x800104E4 because generated C emits sequential unlabeled gotos for 0x800103EC..0x800104E4.
tags: recompiler,jump-table,jr,reentry,toystory2,vagrant-story
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

`_scan_computed_offset` recognized an immediate table base formed by `lui` plus `addiu`, but not the
equivalent `lui` plus `ori` sequence in Toy Story 2. It therefore failed to recover the resident
renderer's `jr (base + ((index & 31) << 3))` control flow and emitted an external dispatch for a
target that is a local `j`/`nop` trampoline.

Accepting `ori` alone exposed a second generic defect: the fallback scanner kept collecting
jump-shaped blocks after the table. The decoded `andi index, source, 31` is an exact 32-slot bound;
ignoring it merged adjacent shared case bodies into the table.

The combined Toy Story 2 regeneration exposed a third generic defect in that candidate. The
reaching-constant map reused `defines_reg()`, a deliberately over-conservative helper designed for
optional vertex-depth attribution. That helper treats an ordinary branch as a possible write to
every queried GPR; the renderer's `bltz` between `lui`/`ori t9,0x800103EC` and the computed `jr`
therefore erased the live table base and silently fell back to external dispatch.

The first candidate then exposed a cross-consumer regression during fresh Vagrant Story TITLE
emission. A duplicated tail contained `jr v0` at `0x8006EF5C`, below its owner's linear range. The
no-index recovery scanned the unrelated owner body, newly counted `lui v0,0x800E` as a target,
and retained that constant past an intervening `lw v0` to fabricate `0x800E066A`. The new
bare-`lui` candidate made this a two-target set and activated the false recovery; walking the
halfword-aligned stream eventually failed at `0x800EFE46`.

## Resolution

The emitter now folds both standard low-half forms through one constant-construction helper and
advances a conservative reaching-constant map with an architectural GPR-write classifier. Ordinary
branches preserve constants; branch-and-link and call instructions write `ra`; loads and ALU
instructions invalidate only their actual destination; unknown instructions clear the map. This is
separate from the intentionally pessimistic depth-attribution predicate. The
no-index heuristic retains its prior contract: only a completed low-half assignment can become a
target; supporting bare-`lui` destinations would require its own control-flow proof. Every recovered
local target must also satisfy the R3000A's four-byte instruction
alignment. For a
left-shifted index whose nearest definition is a contiguous low-bit `andi` mask, it derives the exact
case count from that mask and returns only those fixed-stride targets. Unbounded legacy shapes retain
the existing conservative scan. There is no title, address, seed-list, or generated-code special
case.

The hermetic four-way `lui`/`ori` trampoline test failed before the change by dispatching the first
local slot and returning zero instead of 11. It now places a real branch between constant construction
and dispatch, executes all four cases, requires exactly the four slot labels, and explicitly rejects
the adjacent shared body as a slot. The combined candidate failed this control with no recovered `jr`
until the GPR-write classifier was separated. Two additional RED-first
controls reproduce Vagrant's partial-`lui`/load-clobber false set and reject a masked computed table
with an unaligned base. The emitter suite passes 48/48 and the decoder suite passes 9/9; the decoder suite is
now a normal CTest target, and the full Clang
framework build and 85-test CTest suite pass.

Toy Story 2 was regenerated without the diagnostic `0x8001040C` product seed. Its generated resident
function contains exactly the 32 slots `0x800103EC..0x800104E4`; a bounded real-disc run reaches both
the observed `0x8001040C` slot and the negative sibling `0x800104E4` with no recompilation miss. It
advances to the later independent RenderQueue-capacity boundary.

Vagrant Story's no-argument fresh emission against the same isolated framework now completes 799
resident and 137 TITLE functions instead of aborting in `collect_tail_dups`.

The generic recovery is now landed after the synthetic controls, full framework tests, exact
generated-slot census, and bounded real-disc consumer continuation all passed. Falsifiers are an
extra or missing recovered slot, either measured Toy Story slot reaching external dispatch, a
non-contiguous mask being treated as a case count, an unaligned target being accepted, a low-half
operation using a clobbered partial constant, or a previously supported unbounded computed-offset
idiom losing its targets.
