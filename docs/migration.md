# Native/Lightrec migration

This is psxport's implementation plan. Product intent is `docs/project-goals.md`, factual state is
`docs/project-state.md`, ownership is `docs/codemap.md`, and atomic work is `docs/issues/`.

## Decision

PSX guest execution is settled on Lightrec. This migration does not compare an interpreter against a
JIT and does not design a new MIPS translator. The gameplay product combines native functions and
subsystems with on-demand Lightrec execution of every remaining instruction from the user's original,
authenticated game image.

An interpreter may exist only in a separately built test/oracle target. It is absent from gameplay
objects, links, selectors, configuration, UI, and fallbacks. Lightrec's own interpreted difficult-block
or compile-wait paths are also excluded from the product configuration. If the maintained upstream
cannot provide that boundary, psxport uses a maintained fork containing the required dynarec-only
build/API as ordinary commits and pins its exact revision.

No new work extends the offline MIPS-to-C pipeline. During this migration, do not regenerate, build,
or run the generated-C product as comparison evidence. The original binary, independent emulator or
hardware evidence, and a separately built test oracle provide the references.

## Product runtime

Each live `Core` composes one executor with the following peers:

1. A Lightrec lifetime owner constructs the core with per-`Core` callbacks and executes a bounded
   cycle/instruction budget.
2. A state bridge transfers the complete PSX architectural boundary between `Core` and Lightrec.
3. A code-identity owner authenticates resident/module bytes and assigns a new generation whenever a
   module is loaded or replaced.
4. A native dispatcher resolves the active image-generation-plus-address key, calls an override when
   present, and otherwise executes the original guest body through Lightrec.
5. An invalidation owner normalizes executable write ranges and tells Lightrec which translated code
   is stale.
6. A typed exit result returns control to the host for native/HLE/device work, interrupts/exceptions,
   frame/VSync, cooperative task transitions, budget exhaustion, or a named fault.

Lightrec owns its translated-block cache, chaining, executable-memory allocation/publication, and
teardown. psxport does not place another cache or code-memory wrapper around it. `jit-common` remains a
portfolio authority and source of demonstrated primitives for cores that need them; Lightrec does not.

## State synchronization

At executor entry, `Core` is authoritative. Synchronize:

- 32 GPRs plus HI/LO;
- current PC and any branch-delay/next-PC state required to resume precisely;
- CP0 status, cause, EPC, exception and pending-interrupt state;
- GTE data/control registers and any operation-visible flags;
- guest-cycle position and the bounded deadline; and
- active code identity/generation used by callbacks and native dispatch.

While Lightrec runs, its registered state is authoritative. Before any host callback or exit is
observed, commit all guest-visible state and elapsed cycles to `Core`. After native, HLE, interrupt,
device, or scheduler handling mutates `Core`, reload the corresponding Lightrec state before re-entry.
No helper may read whichever copy happens to be convenient.

The boundary is exercised in both directions with deliberately different values. A test that compares
two zero-initialized states or never enters translated code is invalid.

## Bounded exits

`execute(budget)` returns a typed result containing the reason, guest PC, and consumed budget. Required
reasons are:

- budget exhausted;
- native override requested;
- BIOS/SDK HLE or device service requested;
- interrupt or exception boundary;
- frame/VSync boundary;
- cooperative thread yield or exit;
- guest-requested process exit; and
- unsupported translation, executable-memory, or internal executor fault.

The host handles one result, updates `Core`, and resumes explicitly. A C++ exception never unwinds
through Lightrec frames. Frame or task completion never writes a title phase/timer/scene pointer or
fast-forwards simulation to escape guest code.

## Image-scoped native and original calls

The dispatch identity is `{authenticated image/module, load generation, guest address}`. The resident
image has its own identity; every overlay/module load records identity before execution and advances
the generation even when it reuses the same bytes or address range.

A normal call honors the override registered for the current complete key. A native override's
original call pushes a per-`Core` suppression token for only that key, enters the guest body at the
same address through Lightrec, and restores the token on every typed return/exit. Nested calls still
honor all other overrides. A module with the same address but another identity or generation is never
suppressed accidentally.

The first discriminator uses resident code plus two synthetic/authenticated overlay images that reuse
one guest address. It proves normal, disabled, nested, original-call, unload/reload, and ambiguous-
identity refusal behavior through the production dispatcher.

## Invalidation

All executable-memory writers report exact post-write ranges to one per-`Core` invalidation owner:

- CPU stores and cache-control behavior;
- DMA;
- CD/module loading and decompression;
- debugger/control-channel writes;
- savestate restore; and
- host-native code that intentionally changes executable guest RAM.

The owner normalizes KSEG aliases, checks overlap against executable mappings, updates affected image
generations, and calls Lightrec's supported range/all invalidation API. Override install, remove, or
replace events use the same owner if translated paths can capture the prior dispatch choice. Lightrec
continues to own storage and eviction.

Tests cover changed overlap, unchanged/no-overlap, adjacency, mirrored addresses, cross-boundary
ranges, module-slot reuse, savestate restore, and override-policy changes. Diagnostics always print
ranges scanned, overlaps found, and blocks/decisions invalidated so a silent instrument cannot look
successful.

## Dependency integration

1. Identify the maintained Lightrec upstream/fork revision whose supported host backends cover the
   product matrix.
2. Add it as a direct, immutable dependency with URL, revision, upstream base, license, and fork
   purpose recorded in normal dependency metadata.
3. Build a product library that omits interpreter sources and interpreter fallbacks. Disable threaded
   compile modes that execute an interpreter while waiting; compile synchronously or wait without
   executing guest instructions.
4. Exercise Lightrec's own code-memory and invalidation APIs directly. Do not import the Beetle
   libretro frontend's process-global CPU wrapper and do not depend on its indirectly bundled copy as
   the product pin.
5. Preserve every supported compiler for users. Agent evidence uses Clang; the build does not reject
   GCC or AppleClang solely because of maintainer policy.

## Implementation sequence

### 1. Dependency and link isolation

Resolve issue 0047. Land the direct pin, dynarec-only library, and a two-`Core` translated-execution
test. Add an inspection test that names the product objects/symbols scanned and proves interpreter
code is absent. Do not expose a runtime engine selector.

### 2. State bridge and executor exits

Resolve issue 0048. Implement the production state bridge and typed bounded execution API. Start with
a small authenticated instruction window that exits on budget and a service callback, then cover
interrupt/exception and frame/task exits. Every test executes nonzero Lightrec blocks and includes a
wrong-answer discriminator.

### 3. Image-scoped dispatch

Resolve issue 0049. Implement resident/module identities, generation changes, normal native dispatch,
and scoped original calls. Prove the colliding-overlay discriminator before changing consumers. The
API is runtime-address based; it never manufactures generated-symbol compatibility wrappers.

### 4. Executable invalidation

Resolve issue 0050. Connect every executable-memory writer and override-policy change to one
invalidation owner. Prove both invalidating and non-invalidating cases against the production executor.

### 5. Separate the oracle

Move only still-useful interpreter behavior into a separately built test-oracle target. Its state,
diagnostics, and per-core provenance are test-owned. The gameplay library and consumers do not link it,
and no configuration/UI can select it. Update differential tooling to compare the production Lightrec
boundary against independent evidence without making the test target a product dependency.

### 6. Reference consumer: Tomba! 2

Finish Tomba! 2 before starting another PSX title. Replace its generated-body and wrapper calls with
the runtime normal/original-call APIs, then run the resident plus colliding-overlay discriminator and
reach the current boot-to-gameplay frontier with all existing native owners active. Representative
interactive gameplay must exercise input, native rendering, audio, timing/interrupts, module loads,
native overrides, original calls, and nonzero Lightrec translation.

Only after that integrated gate passes may Tomba! 2 delete its generator, generated corpus/build rules,
seeds, static dispatcher, and static-only tests. A logo, FMV, menu, or headless timing number cannot
authorize deletion or title completion.

### 7. Remove framework static/product selection surfaces

Resolve issue 0051 after the executor contracts are proven. Remove psxport's offline translator,
generated registry/dispatch/provenance interfaces, emission-only tools and docs, gameplay engine enum
and `PSXPORT_ENGINE`, and all product references to the interpreter. Preserve binary analysis tools
whose purpose remains independent of code generation; move them to their actual owner rather than
keeping a compatibility directory.

Update every affected consumer atomically. A fresh consumer checkout must provision from user-supplied
media and build/launch without generated guest code or maintainer-only analysis tools.

### 8. Migrate one title at a time

Tomba! 1 follows Tomba! 2 in their shared title repository. Subsequent PSX titles depend on the shared
executor but are not implementation prerequisites for one another. Record the chosen active title in
its project state and finish its declared representative-gameplay and host gates before beginning
title-specific work in another.

## Retained binary and runtime facts

These facts remain useful because they describe the original title and reached native service
boundaries, not the retired generation process:

- Tomba! 2 retail `SCUS_944.54` is a 167,936-byte boot executable at disc LBA 152155. Its entry is
  `0x80018B6C`, load address `0x80010000`, text size `0x28800`, and initial SP `0x801FFFF0`.
- The boot executable is a loader stub. Root `MAIN.EXE` is the resident program: disc LBA 23,
  716,800-byte file, entry `0x800896E0`, load address `0x80010000`, text size `0xAE800`, initial SP
  `0x801FFFF0`, and GP 0.
- `MAIN.EXE` was measured 99.9% identical to the frame-1000 resident RAM image: 262 differences across
  178,688 compared words, attributable to runtime writes. All 1,596 Ghidra function entries within the
  clean file decoded without an unknown instruction in that audit.
- Tomba! 2 uses a resident core plus overlays loaded above `0x800BE800`; `BIN/*.BIN` modules supply
  address-reusing code/data. This is why runtime dispatch and invalidation require image identity and
  load generation rather than address alone.
- The reached startup path exercised heap and interrupt setup, CD initialization/commands, GPU-ready
  polling, event setup, and CD/VBlank waits. Native CD, synchronous stock-read, CHD-backed sectors,
  device/IRQ, GPU, SPU, GTE, MDEC, XA/FMV, BIOS/SDK HLE, and title-owned frame boundaries remain
  valuable platform owners for the Lightrec product.
- Tomba! 2's native consumer has many generated-body dependencies: the portfolio link audit counted
  816 distinct unresolved body/wrapper symbols across 109 game files with the generated corpus absent.
  Migration replaces those call sites with normal/original runtime dispatch; the count does not imply
  816 new framework wrappers.

The exact title frontier and later evidence belong in the Tomba! 2 project state and issues. Do not
turn this framework plan into a second title-progress ledger.

## Landing gate

The framework dynamic milestone is not complete until all of the following pass on the integrated
tree:

- a fresh Clang/Ninja build consumes the direct immutable Lightrec pin;
- two simultaneous `Core` instances execute nonzero Lightrec blocks without cross-state;
- all state fields and cycles cross every exercised bounded exit correctly;
- resident and colliding-overlay normal/original calls pass with registration-change invalidation;
- executable-write invalidation passes positive and controlled-negative cases;
- the gameplay library and reference consumer link contain no interpreter or generated guest bodies,
  and selector inspection finds no product engine choice;
- the separate test oracle demonstrates both a match and a seeded divergence;
- the reference consumer reaches representative interactive gameplay with its native owners active;
- format, clang-tidy, structure, hermetic tests, configuration/logging ownership, and portability
  checks pass; and
- goals, state, codemap, issues, and user-facing docs match the implemented tree.

This documentation-only planning pass does not satisfy any of those implementation gates.
