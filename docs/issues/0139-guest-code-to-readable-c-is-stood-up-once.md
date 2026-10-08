---
id: 0139
title: Guest code to readable C is stood up ONCE, gated, and it can say a body is missing
status: open
symptom: the operator's directive is "convert the game code into readable C++ until it becomes clear
  why it can't go in-game", and the first step of it existed as three ungated copies -- one per
  repository -- none of which could be shown to fail.
tags: re,decomp,ghidra,tooling,diagnostics
created: 2026-09-28
updated: 2026-09-28
---

## What this is

`tools/decomp_pipeline.py` in the framework, reached from every title as
`external/psxport/tools/decomp_pipeline.py`. One command, given an admitted PS-X EXE and a list of
guest entry addresses, imports the image at that title's real load base, auto-analyses it under a
bounded heap, emits a **function inventory carrying a per-function "is a body actually present"
column**, and decompiles the named targets to C into a git-ignored path.

It replaces, as a fifth thing nobody has to keep in step, `psxport/tools/ghidra_decomp.py`,
`crash/scratch/ghidra-scripts/Decomp.py`, `ctr/scratch/decomp_*.c` and `spyro/tools/decomp_ghidra.py`.

## The one thing it exists to stop: output that reads like a function and is not one

Ghidra's non-returning-function analyzer guesses from call-site shape. On a PSX RAM dump it
mislabels ordinary leaf helpers, and every caller then decompiles to:

```c
/* WARNING: Subroutine does not return */
int FUN_80012340(int a) {

  return 0;
}
```

with the whole body after the call discarded. That is **a function that compiles, has a signature,
has a return, and is wrong.** It is this workspace's signature failure, and it was already paid for
once.

`PSXPORT_CLEAR_NORETURN=all` is the default, and this tool adds the three things the earlier copies
lacked:

1. **It is asserted, not merely requested.** The post-script clears the flag and then READS IT BACK
   over every function. Any function still marked is named by address in the report and the run's
   audit fails. A clear that silently did nothing is the failure, and it used to be invisible.
2. **A body-presence column that is structural, not textual.** See below.
3. **A second, independent signal.** The decompiler's warning wording is Ghidra's and can change, so
   a body is also required to contain a `return`. A C body with no return is not a complete function
   however it is worded.

## The body-presence column, and why "the decompiler returned C" is not the answer

Three disqualifying signals, any one of which fails:

| signal | what it catches |
|---|---|
| no function object at the address | a jump-table-only entry auto-analysis never defined |
| the body holds 0 instructions | a function object over a bodyless region |
| the C carries the non-return warning, or contains no `return` | the truncation above, in either wording |

The instruction count is taken **bounded by the function body's end address**. Ghidra's
`getInstructions(min, True)` walks past the body, so an unbounded count attributes the next function's
instructions to this one and makes a bodyless function look populated — a zero that reads as healthy.

## The image traps it encodes

- **`llvm-objdump --triple=mips` silently misdecodes MMX4's `SLUS_005.61`** and produces confident
  garbage. Any cross-check must use plain `llvm-objdump -d` (LE). Recorded in the manifest entry.
- **Per-title base and text offset are DATA**, in `tools/decomp/manifest.json`, not special cases in
  the logic. Every title in the workspace is a PS-X EXE loading at `0x80010000` with text at file
  offset `0x800`; the Ghidra base is *derived* as `text_load_address - text_file_offset` so it cannot
  drift. Spyro 1's `0x800 + (addr - 0x80010000)` is that formula, verified 62,183/62,183 in its repo.
- **Vagrant Story's SHA-1 matches the CC0 `rood-reverse` decomp's target**, so its symbol addresses
  are confirmed against real bytes. Recorded as a reading aid; the tool copies no decompiled text
  anywhere, and its output path is refused unless git already ignores it.
- **MMX4's vendored decomp is AGPL-3.0 and may not be lifted into `psxport`.** The pipeline has no
  code path that reads a vendored decomp at all, and `_assert_output_dir_is_ignored` refuses to
  write derived C anywhere git would track it.

## Memory, which is the binding constraint on this machine

`~2 GB` free, 16 cores, several agents building concurrently. A Ghidra auto-analysis of a RAM dump
costs 1-2 GB and **an OOM-killed build is reported to its owner as a false red** — the cost of failing
to coordinate is paid by somebody who is not running this tool.

- **Heap ceiling: 1400 MB**, `headless.DEFAULT_MAX_HEAP_MB`, passed as `-X mx1400m`.
  Ghidra's own default is 2 GB, which is the setting that kills a co-tenant.
- **`-X`, not `MAXMEM`.** `MAXMEM` is read by Ghidra's `launch.sh`, which the PyGhidra launcher does
  not go through because it starts the JVM through JPype. A tool that sets `MAXMEM` on this path is
  setting nothing.
- **The lock is `mkdir $PSX/coord/locks/ghidra`**, which is atomic on POSIX: one holder, no daemon, no
  race, released in a `finally` so a refusal never strands the machine. A second run waits with
  backoff and then REFUSES, naming the holder read from the lock's own `holder.txt`. Waiting is
  correct; starting a second analysis is not.
- Ghidra's analysis cache is redirected with `-Djava.io.tmpdir` into the project's own scratch, so a
  few hundred MB of cache never lands on the machine's small tmpfs.

## The launcher traps, each measured here and not assumed

| trap | what it does | what the tool does |
|---|---|---|
| `analyzeHeadless` cannot run a Python script | logs "Ghidra was not started with PyGhidra", prints "Post-analysis succeeded", **exits 0** | uses the `pyghidra.ghidra_launch` module; and requires the inventory file to exist, because exit code is not evidence |
| the `pyghidraRun` wrapper mis-parses | a correct command line arrives as "Bad argument: <project name>", **exits 0** | invokes the module, not the wrapper |
| the Raw Binary loader defines no entry point | 0 instructions, 0 functions, "Analysis succeeded", **exits 0** | a pre-script disassembles **from the image's own declared entry point**, and records that it did so |
| the project directory must pre-exist | "Directory not found" is the whole error | created before the launch |
| a Ghidra script's return value is discarded | a script that "returns 3" continues the run as if it had not checked | both scripts RAISE, which is the only way Ghidra reports a refusal |
| a Python script with `main()` and no call to it | runs, writes nothing, "Post-analysis succeeded", **exits 0** | both scripts have an `if __name__ == "__main__"` entry point, and the selftest checks for it in the source |
| a script that raises leaves Ghidra running anyway | the analysis proceeds and the run reports success | the pre-script records what it did in `preseed.json`; a missing record is a REFUSAL |
| `name in globals()` is False for every GhidraScript name | a "am I inside Ghidra" guard written that way never fires | `__name__ == "__main__"` is the only discriminator used |

**Every one of these is an absence dressed as a success.** That is why the pipeline's hard
requirement is not "Ghidra exited 0" but "the inventory file exists, says 4 of 4 functions scanned,
records a pre-script seed, and every requested target has a body present".

## MEASURED, on the first real run: the tool caught itself twice

**The first successful run returned `AUDIT OK` and was still wrong in two ways.** Recording that is
the point of the exercise, because both defects are the workspace's signature failure reproduced by
the tool meant to catch it.

1. **A target that is a label, not a function entry, was CARVED into a second function.** Asked for
   `0x800258F0` and `0x800259FC`, the tool reported 2 of 2 with a body present. In fact
   **`0x800259FC` is a phase inside `FUN_800258F0`'s own body** (`0x800258F0..0x8002A6FB`, +0x10C),
   and `createFunction` split it: a second 4927-instruction body overlapping the first by 19708 of
   its 19976 bytes, ~129 KB of C each, both "body present", **neither a function**. The fix refuses
   the case and names the enclosing function and the offset into it, because creating a function
   there splits the enclosing body — and the enclosing body is the thing worth reading.
2. **The pre-script raised and the audit passed anyway.** It had seeded disassembly from the start of
   the text window, which on a PS-X EXE is **DATA**: `disassemble(0x80010000)` produced 0
   instructions while the image's own declared entry `0x8005B8E0` produced 670 functions. Ghidra
   logged "Post-analysis succeeded" and exited 0, and the audit had nothing to object to. Two fixes:
   the seed is the image's declared entry point, and the pre-script's work is recorded so its absence
   is a refusal.

**And a third defect, in the diagnostic rather than the tool.** The pre-script's refusal message read
"the load base is the first thing to check". The load base was **correct**; the cause was seeding from
data. That is an instrument putting a plausible cause in the sentence instead of the one it can
support, so the message now states what was scanned and what was tried and explicitly does not say
why.

**And a fourth thing the cross-check got WRONG, recorded because a wrong lead that survives into a
report is worse than no lead.** `tools/decomp/verify_body.py` reads the image, the inventory and the
emitted C and checks the pipeline's own body-presence claim from outside, using the framework's
decoder. Its first version compared the **number of 4-byte words in the body's address range**
against the **instruction count the inventory reports**, and reported Spyro 1's `FUN_800258F0` as a
disagreement: 4,995 words against a claimed 4,994. **That was not a defect in the inventory.** A
function body can hold a word Ghidra does not hold as an instruction — a word of data, or one it
never disassembled — so the two numbers measure different things, `mips.decode` decodes nearly
every bit pattern and so "decodes" that word too, and telling them apart needs Ghidra, which is the
thing the instrument exists to avoid. It was written up as a found bug for a few minutes, which is
exactly the failure this workspace's own findings section names: a specific, satisfying explanation
of a specific number, derived from a comparison that could not have answered the question.

So the two numbers are now printed, each named, and **neither is a verdict**. What the instrument
*can* establish it does assert: the bounds describe a whole number of instructions, the body
**terminates at a return**, and **every direct call site in the bytes has a call in the C** — the
last being the check that catches a caller decompiled with the body after a call discarded, since
that failure loses calls silently.

Measured on the three targets, all three pass those checks:

| target | body | direct call sites | C names | terminates |
|---|---|---|---|---|
| `0x800122A8` | 288 B, 72 instructions | 10 sites, 7 distinct | 8 `FUN_` (7 callees + itself) | `jr ra` + delay slot |
| `0x8001364C` | 3,708 B, 927 instructions | 23 sites, 18 distinct | 19 `FUN_` | `jr ra` + delay slot |
| `0x800258F0` | 19,980 B, 4,994 instructions | 0 sites, 0 distinct | 1 `FUN_` | `jr ra` + delay slot |

`0x800258F0` having **zero direct call sites** is a real and useful reading: 4,994 instructions with
no `jal` at all, so it reaches everything through `jalr` and tail-jumps, and its 130 KB of C is one
enormous function. It is also why the `FUN_`-name comparison cannot be read as "the C called
nothing" — the denominator says the sites are indirect.

## Denominators, and the rule that zero is a refusal

Every count is matched-of-scanned, and the audit **refuses** rather than reports:

- `0 of 0` functions scanned — a run whose script never ran, not an empty image.
- `0 of 0` targets requested — a run that decompiles nothing and is not a result.
- a target with no function — refused and the address named.
- a target that decompiled but has no body — refused, with the reason repeated so it cannot be read
  as "decompile failed".
- a function that decompiled to nothing — distinguished from one that did not decompile at all.

Exit codes mean different things: **0** audited clean, **1** a refusal (missing image, wrong base,
no targets, unreachable Ghidra, lock held), **2** the run completed and its own audit found
something untrustworthy.

## A wrong base is caught before Ghidra starts

The manifest's load address, text file offset and text size are cross-checked against **the image's
own PS-X EXE header**. This matters because a wrong base does not fail on its own: Ghidra imports
happily and every function lands at a plausible address that is not the one the title's documents
name. The refusal names both the claimed and the actual address.

## The selftest, and the evidence it can fail

`tests/test_decomp_pipeline.py` — 121 checks, hermetic (no disc, no image, no Ghidra), driving the
shipping modules through an injected command runner. Both Ghidra-side scripts are importable with no
Ghidra present because they import nothing at module level, so their decision logic is tested
directly.

Every case is a permanent seeded difference: the truncated body in both wordings, a target with no
function, 0-of-0 functions, a non-return flag that survived the clear, a manifest base that disagrees
with the header, an absent image, a raw RAM dump offered as a PS-X EXE, an image whose declared text
runs past its end, an empty target list, an exit-0 run with no inventory, an interpreter that cannot
import PyGhidra, a held lock, and a non-ignored output directory. There is also a **positive** case:
the audit must be CLEAN on a complete run, so a red result above is the seeded difference and not a
test that always fails.

`tests/selftest_must_fail.py` is the weaker, separate evidence: it copies `tools/` and `tests/` into
scratch, removes one defence at a time, and requires the selftest to go red on its own subject.
**Measured: control green, 8 of 8 seeds red.**

Both are registered in ctest, and the framework gate on this branch is **185 of 185 green with
`CMAKE_CXX_COMPILER_ID` "Clang"**, `decomp_pipeline_selftest` at 0.32 s and
`decomp_pipeline_must_fail` at 2.65 s. The count is quoted because "green" without a number is how
`ctest` on an unconfigured directory reads as a pass in this workspace.

**The first gate attempt could not configure, and the reason was staleness rather than this change.**
The branch was cut at `b951d747` and `main` moved 29 commits ahead, re-pinning `shared/lightrec` to
the revision the shared checkout already holds; a branch behind the pin is a build that cannot
configure. Rebasing onto `main` fixed it with no conflict, and the branch is now on top of `main`, so
it can be landed as it stands.

That exercise found a real defect rather than only confirming the plan: the lock had a redundant
`except FileExistsError: pass` in front of an `except OSError` that treated EEXIST identically.
Removing the dead clause left the selftest GREEN, which is how the redundancy was found. It is gone,
and the errno test is now the one the seed removes.

## Falsifier

## Falsifier

This is finished when any of these is observed:

- a run whose audit passes while a decompiled target is missing part of its real body — the
  body-presence column is not doing its job, and the whole tool is a way to produce confident wrong
  C;
- a target reported with a body present whose address lies inside another function's body — the
  carve was not fully removed;
- a run that reports success having written no inventory, or having recorded no pre-script seed;
- a Ghidra auto-analysis of one of these text windows that cannot complete under a 1400 MB heap. The
  number is a measured ceiling, not a guess, and it is the one value here that will need revisiting
  first if the machine's memory budget changes;
- an image whose load geometry the manifest cannot express, which would mean the "data, not code"
  claim is wrong.

## NOT established

- **Only one image has been run end to end through this tool: Spyro 1.** The other eleven manifest
  entries are read from their own PS-X EXE headers, and the header cross-check is what proves those
  numbers, but no full analysis has been performed on CTR, Vagrant, Crash 1/2/3, Crash Bash, MMX4,
  Spider-Man, Tekken 3 or either Tomba! image. Their heap behaviour at these text sizes is UNKNOWN;
  MMX4's text is 3.6x Spyro's and may not fit 1400 MB.
- **Overlays are not covered.** Vagrant's real code is mostly in `.PRG` overlays and Tomba! 2's
  resident executable is ~15% of its title. The manifest has no overlay entries and the reader
  refuses a raw module that is not a PS-X EXE. This is the largest gap in the capability.
- **The decompiler's non-return mislabel has NOT been reproduced here, and that is the most
  important gap in this issue.** The `all` default, the read-back assertion and the two-signal body
  test are the defence, and the read-back is tested; but on Spyro 1 the analyzer marked **0** of 673
  functions, so **no decompilation in this repository has yet been shown to be wrong in the way this
  tool exists to prevent.** The evidence that it "can happen" is inherited from the incident, not
  produced here. It was not separately measured with the policy OFF, so "N of M would have been
  truncated" is not established for any title. The defence is asserted and unexercised, which is
  exactly the state a reader should distrust — and the honest next step is a fixture where the
  analyzer does mark something, so the read-back is seen refusing.
- **`FUN_800258F0` has 0 direct call sites in 4,994 instructions**, so the one large target this run
  produced is entirely `jalr` and tail-jump. The `FUN_`-name comparison cannot be read as "the C
  called nothing" for it; the denominator says the sites are indirect. **Whether its 130 KB of C is
  one function or several that Ghidra merged is not established**, and that is a real open question
  about a function the decomp work order (issue 0135 row 1) calls the S_World renderer.
- **`createFunction` on demand for jump-table-only entries is implemented and reported, but no
  synthetic fixture exercises the jump-table case.** The synthetic fixture's functions are all
  reached by `jal`; the on-demand path is reported by the field, not pinned by a test.
- **`0x800259FC` being a label inside `FUN_800258F0` is this run's finding, not a verified fact
  about the title.** It is what the image says — a second, overlapping function could not be created
  because the address is inside the first body — but nobody has established that the phase there is
  *meant* to be a separate function, and issue 0135 treats it as a region to recover. Whether the
  enclosing function's 130 KB of C is the right unit is a question for whoever reads it.
- **Not a replacement for `spyro/tools/decomp_ghidra.py` yet.** That file is a live agent's work with
  its own selftest; migrating it is a separate change, and until it is done there are two
  implementations in the workspace. The one to retire is spyro's, because this one is the one with
  the body-presence column and the audit.
