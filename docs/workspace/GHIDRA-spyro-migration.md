# Migration note for `spyro/tools/decomp_ghidra.py`

**This is a note for the `spyro` repo's owner. Nothing in `spyro` has been changed by the framework
side, and nothing here should be applied without that owner reading it.** The framework tool is
`psxport/tools/decomp_pipeline.py`; the two do the same job, and the framework one is the one that is
gated, has the body-presence column, and is registered in ctest.

## What is duplicated, and what is not

| concern | `spyro/tools/decomp_ghidra.py` | `psxport/tools/decomp_pipeline.py` |
|---|---|---|
| Ghidra launcher | same conclusion, reached by probe | module invocation, `-X mx<N>m` |
| heap bound | `MAXMEM` | `-X mx<N>m` — **`MAXMEM` is ignored on the PyGhidra path** |
| entry-point seeding | window-start sweep | the image's **declared entry point** |
| no-return clearing | cleared, read-back | cleared, **read back over every function** |
| body presence | not reported | per-function column + audit |
| target with no function | created on demand | created on demand, and a **label inside a body is refused** |
| the lock | none | `coord/locks/ghidra`, waits then refuses, names the holder |
| denominators | partial | every count, and `0 of 0` is a refusal |

**The one line that matters most for a migration:** `MAXMEM` is read by Ghidra's `launch.sh`, which
the PyGhidra launcher does not go through, because it starts the JVM through JPype. A tool that sets
`MAXMEM` on this path is **setting nothing**, and an unbounded heap on this machine is how a
co-tenant's build gets OOM-killed and reported as a false red.

## The migration, in order

1. **Point the repo at the framework tool** rather than copying anything:
   `uv run --frozen python external/psxport/tools/decomp_pipeline.py --image-name spyro1 --image <path> --target <addr> --out scratch/decomp/spyro1`.
   `external/psxport` is the one framework tree, so this is the same path in every port.
2. **Move the per-title geometry into the manifest** if it is not already there. `spyro1` is, with the
   load address, the `0x800` text offset and the text size its own repo measured. If the repo holds
   facts the manifest does not, **add them to `tools/decomp/manifest.json` as a data edit** — do not
   re-derive them in a wrapper.
3. **Keep the repo's own probes.** `tools/probe_guest_disasm.py` and the 62,183/62,183 listing
   cross-check are the title's evidence and are not duplicated by anything in the framework.
4. **Delete `tools/decomp_ghidra.py` and its selftest** in the same change, and delete the
   `HOST_ONLY`/import-failure classification in `scripts/tool_selftests.py` if that repo has one
   naming it.
5. **Take the lock in any remaining hand-run probe** that launches Ghidra, or run it through the
   framework tool so the lock is taken for you.

## What the framework tool does NOT replace

- **Per-title geometry that is not yet in the manifest.** The manifest holds twelve resident titles
  and three Vagrant modules. Overlay entries for `crashbash`, Tomba 1/2 and MMX4 are **not** declared,
  and each needs that port's own **measured** load base and code extent — measured, not guessed.
- **The `psxrecomp`-era static path**, if any of it is still referenced. Nothing in the framework tool
  emits guest C as a build input, and the decompiled output is refused outside a git-ignored path.
- **Anything the repo's own selftest asserts about probe behaviour.** The framework tool's selftest
  gates the pipeline, not the repo's probes.

## Numbers the framework tool has already produced for `spyro1`

From the run on the provisioned `SCUS_942.28` (415,744 B of text, heap 1400 MB):

| | |
|---|---|
| pre-script seed | entry `0x8005B8E0`, **6,040 instructions** |
| functions scanned | **673 of 673**, all holding ≥1 instruction |
| no-return | policy `all`, cleared **0 of 673**, still marked **0** |
| `0x800122A8` | 72 instructions, 725 B of C, `[ret=1 prologue=1 jal=10 jalr=0]` |
| `0x8001364C` | 927 instructions, 11,134 B of C, `[ret=2 prologue=1 jal=23 jalr=1]` |
| `0x800258F0` | 4,994 instructions, 130,139 B of C, `[ret=1 prologue=1 jal=0 jalr=0]` |
| cost | 31.3 s wall, **1,104,476 KB peak** on one run, 694,724 KB on another |

**`0x800259FC` is NOT a function entry** — it is a phase inside `FUN_800258F0`'s body, `+0x10C` in. An
earlier version of the framework tool carved it into a second 4,927-instruction body overlapping the
first by 19,708 of its 19,976 bytes; both reported a body present and neither was a function. It is
refused now, with the enclosing function named. If a doc or probe in `spyro` treats `0x800259FC` as an
entry, that is worth correcting.

And `FUN_800258F0` has **1 prologue and 1 return** in 4,994 instructions, so it is **one function, not
several the analyzer merged** — a very large one, 130 KB of C, that reaches everything by branch.
