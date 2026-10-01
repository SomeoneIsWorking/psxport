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
    --image-name spyro1 \
    --image scratch/assets/spyro1/SCUS_942.28 \
    --target 0x800258F0 \
    --out scratch/decomp/spyro1
```

`--list-titles` prints every image with its kind, base, window and SHA-1. **Point the other agents
at this.** Per-title geometry is a JSON edit in `tools/decomp/manifest.json`, not code.

## Ask questions about the image, many per run, one Ghidra launch

Reading a title one address at a time is what the analysis cache and the query flags exist to stop.

```sh
uv run --frozen python external/psxport/tools/decomp_pipeline.py \
    --image-name spyro2 --image scratch/assets/spyro2/SCUS_944.25 \
    --callers 0x80044504 \        # who CALLS it
    --refs 0x800A11E4 \           # what READS or WRITES it, with the access and the function
    --function-at 0x80044AE0 \    # which function contains this PC
    --out scratch/decomp/spyro2
```

Each flag is repeatable, all of them are answered in the same launch as any `--target`, and each line
of the answer states what was scanned:

    [decomp] queries: 3 asked, 3 answered, against memory 0x8000F800..0x80066FFF of SCUS_944.25
    (614 function(s) in the program). A count is references the ANALYZER holds: this is static
    cross-reference scope, not runtime execution.
      callers 0x80044504: 1 reference(s) to it (1 code, 0 data) from 1 function(s)
        0x8004C564 UNCONDITIONAL_CALL control   8004C534 FUN_8004c534   jal 0x80044504
      refs 0x800A11E4: 20 reference(s) to it (0 code, 20 data) from 8 function(s)
        0x8003B350 WRITE other 8003B33C FUN_8003b33c  sh v0,0x11e4(at)
      function_at 0x80044AE0: +1500 bytes inside FUN_80044504, 0x80044504, body 0x80044504..0x80046FD7

Three things the answers will NOT do, because each would be a confident answer about the wrong thing:

- an address **outside the program's memory** is answered as outside memory, never as a bare "0
  references" — no reference to it could exist, and that is a different claim;
- an answer whose **count disagrees with the sites it lists** is refused, not printed;
- a question **asked and not answered** is refused by name, so a dropped question cannot read as a
  question with no result.

## The analysis is cached, and the run says which mode it was

The Ghidra project for an image is kept under the **consuming repository's**
`build/ghidra/<image>/<sha256>/` — keyed by the image's SHA-256, so a different region or revision
cannot answer as if it were this one. The first run imports and analyses; later runs open it with
`-process -noanalysis`:

    [decomp] analyze run in 19.4 s; project <repo>/build/ghidra/spyro2/7b54002… (7b54002789ab379e), analyzed=True
    [decomp] reuse   run in  3.8 s; project <repo>/build/ghidra/spyro2/7b54002… (7b54002789ab379e), analyzed=True

A directory with **no recorded state** is a run that died, not an analysis: it is re-analyzed rather
than opened. `--fresh` discards the slot and analyzes again, and it removes only a directory this tool
created under `build/ghidra/`. A warm run passes **no pre-script** — re-seeding overwrites the analysis
run's record with a different number (Spyro 2's entry reaches 4,753 instructions before analysis and
11,674 after) and both print as "the pre-script seed"; the recorded seed is replayed instead, labelled
as replayed.

## Overlays and modules — the titles that are actually blocked

**For Vagrant Story, Crash Team Racing, both Tomba! titles and Mega Man X4, the code on the path of
the defect is in an OVERLAY, not in the main executable.** Vagrant's boot exe is ~15% of the code;
933,925 B lives in `.PRG` overlays. A resident-only reader cannot see any of it, and it is the wall
those titles are stuck behind.

A module is not a PS-X EXE. It has **no header, no entry point, and no address derivable from the
file**, so its load base and code window are the **owning port's measured facts** — declared in the
manifest, as data:

```sh
uv run --frozen python external/psxport/tools/decomp_pipeline.py \
    --image-name vagrant_battle \
    --image scratch/bin/overlays/BATTLE.BIN \
    --target 0x800760CC \
    --out scratch/decomp/vagrant_battle
```

Measured on `BATTLE.BIN` (577,828 B, SHA-1 `d53aaccc…`, the decomp's own declared target):

| | |
|---|---|
| load base | `0x80068800` (vagrant's measured fact) |
| pre-script seed | entry `0x80069C6C`, **629 instructions** |
| functions scanned | **1,709 of 1,709**, all holding ≥1 instruction |
| targets | requested 2, found 2, decompiled 2, body present 2 |
| `FUN_800760CC` | 116 instructions, 1,262 B of C — the port's projection owner |
| cost | 24.7 s wall, **734,116 KB peak RSS** at a 1400 MB heap |

`kind` is a **field, not a branch in the code**: `ImageSpec` carries whichever fields apply and both
kinds go through one offset formula, so an overlay and a resident executable are read by ONE
implementation rather than two that drift.

**What is checked, and by what.** A resident is cross-checked against its own header. A module cannot
be, so:

- **SHA-1 gate.** A module's base is trusted as data, so the only thing stopping a wrong *file* is its
  hash. A mismatch is a refusal naming the hash it computed.
- **the code window must lie inside the file**, and the base must be in 2 MiB of KSEG0;
- **`code_first` is the measured CODE EXTENT, not 0.** This is measured, not stylistic: BATTLE's
  first words are a **pointer table** (`0x8006C684` = base+0x3E84, and eight consecutive words all
  resolve inside the module), so seeding at file offset 0 reaches nothing, while offset `0x146C`
  holds `0x27BDFFE8` — an `addiu $sp,$sp,-N` prologue, which is what a code extent starts with.
- **the base is shared between modules.** BATTLE and TITLE both load at `0x80068800`; INITBTL loads
  at `0x800F9800`. A guest address is meaningless without the module identity, so `--image-name` is
  what selects the geometry and the report prints it.

### What a wrong load base costs, measured

Not "it fails" — the number, from `tests/controls_overlay.py`:

| base | measured addresses still covered |
|---|---|
| one page (`0x10000`) high | **0 of 3** |
| the module→resident distance (`0x48800`) | **0 of 3** |
| **`0x800` low** (the resident text-offset error) | **2 of 3** |

**And the last row is the finding: coverage is NOT a sufficient detector.** A base 0x800 low still
covers both code addresses, so a reader that only asked "does my window cover this target?" would
accept it and report every address one page out. What does catch it is the pipeline's own seed: at a
wrong base the declared entry holds **non-code** (`0x8006946C` → `0x800A3FA8`, and
`0x80079C6C` → `0x14620009`), and the pre-script raises when disassembly from the declared entry
yields zero instructions.

### The `lui` minus-displacement trap

Vagrant's own words build `vs_main_dispEnv` at `0x8005E188` as `lui $s0, 0x8006` + `addiu $s0, $s0,
-7800` — a **`lui` page MINUS a displacement**. A reader that forms an address as `lui + positive`
gets `0x8006E188`: wrong by exactly `0x10000`, and nothing about the result says so.

The decompiled `FUN_800760CC` shows the same shape in its last two statements, which is why the trap
is reachable from the C and not only from the bytes:

```c
func_0x80028e80(_DAT_8005e210 * 0x14 + -0x7ffa1e78);
func_0x80028cb4(_DAT_8005e210 * 0x5c + -0x7ffa1f30);
```

Both strided by a base written as a **negative** displacement. And note `0x8005E188` is **below** the
module's own load base, so a reader that bounds its window by the base alone refuses a legitimate
address. The controls assert all three facts.

## "One function, or several the analyzer merged?"

A Ghidra function can absorb code it never calls, and then its C is a wall of unrelated C presented
as one function. Nothing in Ghidra's API says whether that happened, so every target row now carries
the **counts a reader needs, and no verdict**:

```
0x800258F0  FUN_800258f0  ... insns=4994 c=130139B [ret=1 prologue=1 jal=0 jalr=0]  ...
```

`ret` is jump-register forms, `prologue` is `addiu $sp,$sp,-N` forms, `jal`/`jalr` separate direct
from indirect calls. **Many prologues inside one body is the shape of a merge**; one is a single
entry. A threshold would be a guess about an image, so there is none — the numbers are the evidence
and the judgement is the reader's.

These are read on the body's **own instruction grid**. An earlier version stepped by 4 from
`body_first` and reported **0 returns** for a body whose last two words are plainly `jr ra` + `nop`;
that was a bug in the analysis, and it is the reason the grid comes from `getBody()`.

Measured on Spyro 1's `FUN_800258F0` — 4,994 instructions, 19,980 bytes:

| | |
|---|---|
| prologues | **1** — one entry point |
| returns | **1**, and the body's last two words are `jr ra` + `nop` after a full `lw` restore of `s0..s3` |
| direct / indirect calls | **0 / 0** — it reaches everything by branch |

**One prologue and one matching epilogue, so this is ONE function, not several merged** — a very
large one that tail-jumps throughout. It is 130 KB of C because it is 4,994 instructions of renderer.

## The non-return defence, EXERCISED

On Spyro 1 and on a purpose-built fixture, Ghidra's "Non-Returning Functions - Discovered" analyzer
**ran and marked nothing** — its row is in the analyzer table, 0 marked of 673 functions. So the
defence could not be provoked through auto-analysis, and the fixture **sets the flag itself** on the
one function that genuinely never returns, then measures both halves over the same caller in one
process (`tests/noreturn_fixture_run.py`, a ctest that launches Ghidra):

| caller | phase | bytes | statements | warning |
|---|---|---|---|---|
| `0x800100C0` | un-cleared | 111 | 2 | **present** |
| `0x800100C0` | cleared | 59 | 3 | absent |
| 3 others | either | 49–59 | 2–3 | absent |

The un-cleared C, in full — this is the workspace's signature failure, reproduced on purpose:

```c
void FUN_800100c0(void) {
  /* WARNING: Subroutine does not return */
  FUN_80010000();
}
```

and the cleared C, same caller, same process:

```c
void FUN_800100c0(void) {
  FUN_80010000();
  return;
}
```

**1 of 4 callers truncated, the clear removed it from all 4, and the body came back in the truncated
one.** The 3 untruncated callers are the control: the warning is a property of the flagged call site,
not of every call in the fixture.

## What you get

```
[decomp] image=vagrant_battle kind=module program=BATTLE.BIN language=MIPS:LE:32:default
[decomp] pre-script seed: entry=0x80069C6C instructions=629
[decomp] no-return policy=all cleared=0 of 1709 functions; still marked after clear=0
[decomp] functions scanned=1709, of which 1709 hold at least one instruction
[decomp] targets requested=2, function found=2, decompiled=2, body present=2
[decomp] function inventory (1709 scanned):
  entry       name                     body      insns   body range
  80068800    FUN_80068800            16B       4       80068800..8006880F
  ...
[decomp] targets (2 requested):
  0x800760CC  FUN_800760cc  found=True decompiled=True body=True insns=116 c=1262B [ret=1 prologue=1 jal=0 jalr=0]  ...
[decomp] AUDIT OK: no-return cleared on 0 of 1709 functions, 2 of 2 targets carry a real body.
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
| anything to ask at all | `nothing was asked … A run with neither decompiles nothing, answers nothing, and is not a result.` |
| an unreadable query line | refused with the LINE NUMBER and the token: a line that cannot be read is a question that would be dropped rather than answered. |
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
  kills a co-tenant. **MEASURED to hold on the largest image in the manifest**: MMX4, 1,177,600 B of
  text (2.83x Spyro's), 4,834 functions, **1,089,096 KB peak** across two runs — 78% of the ceiling.
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

