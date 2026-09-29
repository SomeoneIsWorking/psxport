# Guest code to readable C — the one command

**The capability the operator's directive needs first, stood up once for every title.** Given an
admitted PSX image and a list of guest entry addresses, it imports the image at that title's real
load base, auto-analyses it under a bounded heap, emits a function inventory that says whether each
function's **body is actually present**, and decompiles the targets to C under a git-ignored path.

The tool is `tools/decomp_pipeline.py`. Every title reaches it as
`external/psxport/tools/decomp_pipeline.py`, because each title's `external/psxport` is the one
framework tree.

## The one command

```sh
uv run --frozen python external/psxport/tools/decomp_pipeline.py \
    --title spyro1 \
    --image scratch/assets/spyro1/SCUS_942.28 \
    --target 0x800258F0 --target 0x800259FC \
    --out scratch/decomp/spyro1
```

`--out` **must** be under a git-ignored path. The tool asks `git check-ignore` before it writes
anything and refuses otherwise: decompiled guest C is a reading aid, never a build input, and this
workspace must never commit it — least of all MMX4's AGPL-3.0 decomp or anyone else's text.

See what the manifest knows before choosing a base:

```sh
uv run --frozen python external/psxport/tools/decomp_pipeline.py --list-titles
```

## What you get

```
[decomp] program=SCUS_942.28 language=MIPS:LE:32:default
[decomp] no-return policy=all cleared=NNN of NNN functions; still marked after clear=0
[decomp] functions scanned=NNNN, of which NNNN hold at least one instruction
[decomp] targets requested=2, function found=2, decompiled=2, body present=2
[decomp] function inventory (NNNN scanned):
  entry       name                     body      insns   body range
  80010020    FUN_80010020            16B       4       80010020..8001002F
  ...
[decomp] targets (2 requested):
  0x800258F0  FUN_800258F0   found=True decompiled=True body=True insns=NNN c=NNNNNB  ...
[decomp] AUDIT OK: no-return cleared on NNN of NNN functions, 2 of 2 targets carry a real body.
```

**Exit codes mean different things, and the difference is the point.**

| code | meaning |
|---|---|
| 0 | the run completed and its audit found nothing untrustworthy |
| 1 | a **refusal** — absent image, wrong base, no targets, unreachable Ghidra, another Ghidra held the lock |
| 2 | the run completed and its own audit found something **untrustworthy** — a target with no body, or a non-return flag still set |

A run that established nothing never reports success.

## Why the "body present" column exists

Ghidra's non-returning-function analyzer guesses from call-site shape. On a PSX RAM dump it
mislabels ordinary leaf helpers, and every caller then decompiles to:

```c
/* WARNING: Subroutine does not return */
int FUN_80012340(int a) {

  return 0;
}
```

with the whole body after the call discarded. **That is a function that compiles, has a signature,
has a return, and is wrong.** Reading it as a finished function is this workspace's signature
failure, and it has already been paid for once.

So the tool does three things the earlier ad-hoc copies did not:

1. **`PSXPORT_CLEAR_NORETURN=all` is the default AND is asserted.** The post-script clears the flag
   and then reads it back over every function. Anything still marked is named by address and the
   audit fails. A clear that silently did nothing is the failure.
2. **A structural body-presence column**, not a textual one: a function object must exist, its body
   must hold at least one instruction, and its C must carry no non-return warning **and** contain a
   `return`. The `return` test is a second, independent signal, because the warning's wording is
   Ghidra's and can change.
3. **Instruction counts bounded by the body's end address.** Ghidra's `getInstructions` walks past the
   body, so an unbounded count hands the next function's instructions to this one and makes a
   bodyless function look healthy.

When `body=False`, the reason is printed with the target, so a refusal cannot be read as "decompile
failed".

## Refusals are not pedantry

| you forgot | what you get |
|---|---|
| the image | `no image at <path>` |
| a title | `no manifest entry titled 'x'. Known titles: … (12 known)` |
| `--target` | `no target addresses … A run with no targets decompiles nothing and is not a result.` |
| a correct base | the manifest's load address and text size are checked against **the image's own PS-X EXE header**; a disagreement is refused, naming both values. A wrong base does **not** fail on its own — Ghidra imports happily and every function lands at a plausible address that is not the one your documents name. |
| PyGhidra | named, with the interpreter that would actually launch Ghidra |
| the lock | the holder, read from the lock's own `holder.txt` |
| a git-ignored `--out` | refused before a byte is written |

## Adding a title is a data edit

`tools/decomp/manifest.json` holds the per-title load geometry. Adding a title is one entry, not a
code change, which is the difference between a shared capability and a sixth private copy:

```json
"mytitle": {
  "serial": "SLUS_999.99",
  "text_load_address": "0x80010000",
  "text_file_offset": "0x800",
  "text_size": "0x12345",
  "note": "what a reader needs to know before trusting an address here"
}
```

The Ghidra base is **derived** as `text_load_address - text_file_offset`, so it cannot drift. For
Spyro 1 that is `0x80010000 - 0x800 = 0x8000F800`, which is the formula its repo verified against
62,183 of 62,183 recorded instructions.

**Overlays are not covered.** Vagrant's real code is mostly in `.PRG` overlays and Tomba! 2's
resident executable is ~15% of its title; there are no manifest entries for them, and the reader
refuses a raw module that is not a PS-X EXE. That is the largest known gap.

## Memory is the binding constraint, so the tool coordinates

Roughly 2 GB free, several agents building at once. A Ghidra auto-analysis of a RAM dump costs
1-2 GB, and **an OOM-killed build is reported to its owner as a false red** — the cost lands on
somebody who is not running this tool.

- **Heap ceiling 1400 MB** (`-X mx1400m`). Ghidra's own default is 2 GB, which is the setting that
  kills a co-tenant.
- **`-X`, not `MAXMEM`.** `MAXMEM` is read by Ghidra's `launch.sh`, which the PyGhidra launcher does
  not use because it starts the JVM through JPype. Setting `MAXMEM` here sets nothing.
- **One Ghidra at a time**, via `mkdir $PSX/coord/locks/ghidra`. `mkdir` is atomic on POSIX, so the
  lock needs no daemon and has no race. A second run waits with backoff and then refuses, naming the
  holder. Override the location with `--lock-dir`, and the patience with `--lock-wait`.
- Ghidra's cache is redirected into your own scratch, never the machine's small tmpfs.

## The launcher traps, measured

Every one of these is an **absence dressed as a success**, which is why the pipeline's real
requirement is "the inventory file exists and says 4 of 4" rather than "Ghidra exited 0".

| trap | what it does |
|---|---|
| `analyzeHeadless` cannot run a Python script | logs "Ghidra was not started with PyGhidra", prints "Post-analysis succeeded", **exits 0** |
| the `pyghidraRun` wrapper mis-parses positionals | a correct command line arrives as "Bad argument: <project name>", **exits 0** |
| the Raw Binary loader defines no entry point | 0 instructions, 0 functions, "Analysis succeeded", **exits 0** — a pre-script disassembles the manifest's window instead |
| a Ghidra script's return value is discarded | a script that "returns 3" continues as if it had not checked; both scripts raise instead |
| the project directory must pre-exist | "Directory not found" is the whole error |

This tool uses the `pyghidra.ghidra_launch` **module**, not the wrapper, and requires the inventory
to exist afterwards.

## Checking the C against the image, from outside the tool

The body-presence column is the pipeline's **claim** about its own output. A claim is not evidence, so
there is a second instrument that reads the image, the inventory and the emitted C and checks the
claim from outside:

```sh
uv run --frozen python external/psxport/tools/decomp/verify_body.py \
    --inventory scratch/decomp/spyro1/inventory.json \
    --c scratch/decomp/spyro1/c/800122A8.c \
    --address 0x800122A8 \
    --image scratch/assets/spyro1/SCUS_942.28
```

It decodes the body's exact instruction range with the framework's own decoder
(`tools/mips/decode.py`), through the manifest's file-offset formula, and asserts the three things it
can actually establish: the bounds describe a **whole number of instructions**, the body **ends at a
return**, and **every direct call site in the bytes has a call in the C**. The last is the one that
catches a caller decompiled with the body after a call discarded — that failure loses calls,
silently.

It prints the decoded listing, because it **cannot** establish that the C is the semantically
correct program. Only reading both does that, and it is expected of you. A pass here is "the
structure agrees", not "the function is right".

**It deliberately does NOT check the instruction count**, and that is a correction rather than an
omission. It once compared the words in the body's address range against the count the inventory
reports and flagged Spyro 1's `FUN_800258F0` as 4,995 against a claimed 4,994. That was not a
defect: a body can hold a word Ghidra does not hold as an instruction, so the two numbers measure
different things, and telling them apart needs Ghidra. The two are printed, each named, and neither
is a verdict.

**A `jalr` is excluded from the call comparison** and the denominator says so — Spyro 1's
`FUN_800258F0` has 4,994 instructions and **0 direct call sites**, reaching everything through
`jalr` and tail-jumps. Without the exclusion that would read as "the C called nothing", which is a
statement about the scan, not about the function.

## What it is not

- **Not a build input and not a shipping path.** The decompiled C is a reading and porting aid. What
  ships is a hand-written native override that owns the recovered behaviour, with the JIT executing
  everything else. Nothing here emits guest functions as C, objects, or a precompiled substrate.
- **Not a decompilation corpus.** It does not walk the image; it reads the targets you name.
- **Not a symbol recovery pass.** An entry reached only through a jump table has no call site for
  auto-analysis to find; the post-script creates such a function on demand and the report says
  `created_on_demand` when it did.
- **Not yet run on all twelve titles.** Only Spyro 1 has been run end to end; the other manifest
  entries are proven by the header cross-check, not by a completed analysis. See
  `docs/issues/0137-guest-code-to-readable-c-is-stood-up-once.md` for the full "not established"
  section, which is the honest limit of this capability today.
