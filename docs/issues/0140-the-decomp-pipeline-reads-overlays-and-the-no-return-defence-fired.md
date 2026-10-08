---
id: 0140
title: The decomp pipeline can read overlays, and the no-return defence has been seen to fire
status: open
symptom: the shared decomp pipeline stood up in 0139 read only a resident PS-X EXE, so the titles
  that are actually blocked could not use it — Vagrant Story's projection owner `func_800760CC`
  lives in `BATTLE.PRG`, an overlay with no header. Separately, the pipeline's no-return defence was
  ASSERTED AND UNEXERCISED: on Spyro 1 the analyzer marked 0 of 673 functions, so nothing had ever
  been seen to fail.
tags: re,decomp,ghidra,overlays,diagnostics
created: 2026-09-29
updated: 2026-09-29
---

## Overlays, and the wall they were behind

A `.PRG`/`.BIN` overlay is **not** a PS-X EXE: no header, no entry point, and no address derivable
from the file. The 0139 reader refused one, so the code on the path of the visible defects — Vagrant's
933,925 B of overlay code, CTR's and Tomba's overlay-scoped work — was unreachable.

`ImageSpec` now carries a `kind` field, and **it is a field rather than a branch in the code**: the
fields that apply to each kind are carried, and both go through one offset formula, so an overlay and
a resident executable are read by ONE implementation. Per-title geometry is still a manifest edit.

**What a module cannot be checked against, and what stands in for it.** A resident is cross-checked
against its own header. A module has none, so:

- **the SHA-1 gate** is the only thing stopping a wrong *file*, and it is measured against the decomp's
  own declared target for all four Vagrant images;
- **the code window must lie inside the file**, and the base must be in 2 MiB of KSEG0;
- **`code_first` is the measured CODE EXTENT, not 0.** MEASURED: `BATTLE.BIN`'s first words are a
  **pointer table** — `0x8006C684` is base+`0x3E84` and eight consecutive words all resolve inside the
  module — so seeding disassembly at file offset 0 reaches nothing, while offset `0x146C` holds
  `0x27BDFFE8`, an `addiu $sp,$sp,-N` prologue. That is what a code extent starts with, and the two
  repos' own tools use the same shape to find entry offsets.

**MEASURED, `BATTLE.BIN`** (577,828 B, SHA-1 `d53aaccc…`, vagrant's measured base `0x80068800`):

| | |
|---|---|
| pre-script seed | entry `0x80069C6C`, **629 instructions** |
| functions scanned | **1,709 of 1,709**, all holding ≥1 instruction |
| targets | requested 2, found 2, decompiled 2, **body present 2** |
| `FUN_800760CC` | 116 instructions, 1,262 B of C — the port's projection owner |
| cost | 24.7 s wall, **734,116 KB peak RSS** at a 1400 MB heap |

**THE CONTROL, because a reader that only ever succeeds cannot fail.** `tests/controls_overlay.py`
puts a resident EXE and a module through the SAME `ImageWindow` and checks the two offset formulas
disagree while both stay live — one reader, two kinds, **18 checks**. The overlay path comes from
`PSXPORT_VAGRANT_BATTLE` and is **never hardcoded**, because a committed absolute home path is both a
machine-specific path in a tracked file and a control that would pass on one machine and skip
everywhere else. With the overlay absent the run reports **"CONTROLS PARTIAL: 3 of 18 checks ran"** —
a skipped comparison is not a passing one, and ctest's exit code cannot tell them apart, so the
denominator is what a reader has to notice.

### What a wrong load base costs, counted — and why coverage is NOT enough

| base | measured addresses still covered |
|---|---|
| one page (`0x10000`) high | **0 of 3** |
| the module→resident distance (`0x48800`) | **0 of 3** |
| **`0x800` low** (the resident text-offset error) | **2 of 3** |

**The last row is the finding.** A base `0x800` low still covers *both code addresses*, so a reader
that only asked "does my window cover this target?" would accept it and report every address one page
out. What catches it is the pipeline's own seed: at a wrong base the declared entry holds **non-code**
(`0x8006946C` → `0x800A3FA8`; `0x80079C6C` → `0x14620009`), and the pre-script raises when
disassembly from the declared entry yields zero instructions.

### The `lui` minus-displacement trap, on the module's own words

`vs_main_dispEnv` at `0x8005E188` is `lui $s0, 0x8006` + `addiu $s0, $s0, -7800` — a **`lui` page MINUS a
displacement**. A reader forming an address as `lui + positive` gets `0x8006E188`: **exactly `0x10000`
out, and nothing about the result says so.**

The decompiled `FUN_800760CC` carries the same shape in its last two statements, so the trap is
reachable from the C and not only the bytes:

```c
func_0x80028e80(_DAT_8005e210 * 0x14 + -0x7ffa1e78);
func_0x80028cb4(_DAT_8005e210 * 0x5c + -0x7ffa1f30);
```

Note also that `0x8005E188` is **below** the module's own load base, so a reader bounding its window
by the base alone refuses a legitimate address. All three facts are asserted.

## The no-return defence, EXERCISED

**It had never fired.** On Spyro 1 the analyzer marked 0 of 673, and on a purpose-built fixture the
"Non-Returning Functions - Discovered" analyzer **ran** — its row is in the analyzer table — and marked
**0 of 6**. So the mislabel could not be provoked through auto-analysis, and the fixture **sets the
flag itself** on the one function that genuinely never returns, then measures BOTH halves over the
SAME caller in ONE process.

| caller | phase | bytes | statements | warning |
|---|---|---|---|---|
| `0x800100C0` | un-cleared | 111 | 2 | **present** |
| `0x800100C0` | cleared | 59 | 3 | absent |
| 3 others | either | 49–59 | 2–3 | absent |

The un-cleared C, in full — the workspace's signature failure, reproduced on purpose:

```c
void FUN_800100c0(void) {
  /* WARNING: Subroutine does not return */
  FUN_80010000();
}
```

and after the clear, same caller, same process: `FUN_80010000(); return;`. **1 of 4 callers
truncated, the clear removed it from all 4, and the body came back in the truncated one.** The 3
untruncated callers are the control: the warning is a property of the flagged call site, not of every
call in the fixture.

**FOUR attempts failed first, and each produced a clean-looking zero from an instrument that had
scanned nothing** — the dead-tap family, inside the instrument built to end that family:

1. a linear sweep of the fixture decoded **2 instructions of 336** and found 0 functions, because the
   fixture opens with an infinite branch loop the sweep entered and never left;
2. seeding only the spinner's entry left **0 functions**, so the post-script decompiled nothing and
   every row read 0 bytes — which reads as "the failure did not reproduce" and is "there was nothing
   to decompile";
3. `getAnalysisOptions` does not exist as a GhidraScript method; asking for it raises NameError and
   leaves the pre-script unrun while the run reports success. (The same trap the spyro pre-script
   recorded.)
4. a first version of the assertion demanded the clear restore a body for **every** caller, and three
   of four were never truncated — so it failed on callers the defence had nothing to do with, which is
   a check that cannot fail for the right reason.

## `FUN_800258F0`: one function, not several merged

Every target row now carries the **counts a reader needs, and no verdict**: `ret` (jump-register
forms), `prologue` (`addiu $sp,$sp,-N`), `jal`, `jalr`. Many prologues inside one body is the shape
of a merge; one is a single entry. **A threshold would be a guess about an image, so there is none.**

Measured on Spyro 1's `FUN_800258F0` — 4,994 instructions, 19,980 bytes:

| | |
|---|---|
| prologues | **1** |
| returns | **1** — the last two words are `jr ra` + `nop`, after a full `lw` restore of `s0..s3` |
| direct / indirect calls | **0 / 0** — it reaches everything by branch |

**One prologue and one matching epilogue: this is ONE function, not several merged.** It is 130 KB of
C because it is 4,994 instructions of renderer. 25.3 s wall, 694,724 KB peak.

**The property shipped a confident WRONG number twice before that answer was right**, both recorded
in the code and both now permanent checks:

- it reported **`ret=0`** for a body whose last two words are plainly `jr ra` + `nop`, because the
  count matched only funct `0x09` (`jalr`) and not `0x08` (`jr`);
- it read the body stepping by 4 from `body_first`, which is the body's grid only when `body_first`
  is 4-aligned, and it used `listing.getByteAt`, which **does not exist** — an AttributeError that
  killed the post-script while Ghidra logged "Post-analysis succeeded" and exited 0. The pipeline's
  missing-inventory refusal is what caught it.

## MMX4's real memory ceiling, measured rather than assumed

0139 flagged MMX4's text as possibly too large for the 1400 MB ceiling. **It fits.**

| | MMX4 | Spyro 1 (control) |
|---|---|---|
| text window | 1,177,600 B | 415,744 B (**2.83x**) |
| functions scanned | **4,834 of 4,834** | 673 of 673 |
| pre-script seed | entry `0x800DAE8C`, **6,614 instructions** | 6,040 |
| peak RSS | **1,089,096 KB** at a 1400 MB heap (two runs: 931,068 and 1,089,096) | 694,724 KB |
| wall | 53.3 s | 25.3 s |

**The 1400 MB ceiling HOLDS on the largest image in the manifest**, peaking at 1.09 GB across two
runs — 78% of the ceiling. A correction to 0139's own note: MMX4's text is **2.83x** Spyro's, not the
3.6x stated there.

**AND A REAL FINDING, about the addresses rather than the memory.** Two MMX4 addresses that older
notes treat as function entries are not, and the tool refused both:

- **`0x80015FE0`** has no function: the nearest entry is `0x80016004`, **0x24 higher**.
- **`0x80015FD4`** is **+264 bytes inside `FUN_80015ecc`'s body** (`0x80015ECC..0x80016003`) — a label
  reached by a branch, not an entry.

So the pair the megamanx4 notes call the appender's cursor and its back-edge are **inside one
function**, and the entry for that work is `0x80016004` (28 instructions, 423 B of C, `[ret=1
prologue=1 jal=1]`). The run **exited 2**, which is the correct refusal, and it named the enclosing
function and the offset rather than saying "not code". Anyone working MMX4's decompression path should
target `0x80016004` and treat `0x80015FD4`/`0x80015FE0` as **not entry points**.

## Falsifier

- a module run whose functions land at addresses the port's own tools do not name — the base or the
  extent is wrong and the SHA-1 gate cannot see it;
- a wrong base accepted because its window covered the targets;
- the no-return fixture ceasing to reproduce the warning, which would mean the decompiler changed and
  the fixture no longer exercises the failure it exists for;
- a `ret`/`prologue` count that disagrees with the image's own bytes.

## NOT established

- **No-return on a REAL image is still unmeasured.** The fixture sets the flag itself; the analyzer
  has not been observed to mislabel a real PSX title. So "N of M functions on title X would have been
  truncated" remains unestablished for every title, and the defence is now proven to WORK when the
  flag is present, not proven to be NEEDED at a rate.
- **Only three images have been run end to end**: spyro1, vagrant_battle, megamanx4. The other
  manifest entries are proven by the header cross-check, not by analysis.
- **Only 3 of Vagrant's 20 modules are declared.** `crashbash`'s 7 overlays, Tomba 1/2's and MMX4's
  are not in the manifest; each needs its owning port's measured base, which is the same kind of work
  and is not automated.
- **The `lui` trap is demonstrated on Vagrant's bytes and not surveyed elsewhere.** Another title
  could form addresses a third way.
- **No overlay-to-overlay identity check exists.** BATTLE and TITLE share a base, so an address is
  meaningless without the module name; the tool reports the name but does not stop a caller mixing two.
