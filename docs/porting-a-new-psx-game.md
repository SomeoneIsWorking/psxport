# Porting a new PlayStation game with psxport

This guide describes the native/Lightrec architecture. psxport's current code is still migrating to
it; follow `docs/project-state.md` and do not begin a new title until the active PSX title's declared
conformance gate is complete.

## What the framework provides

- A per-`Core` Lightrec executor for non-native MIPS R3000A code.
- PSX GPU, SPU, GTE, MDEC, CD/DMA/timer/pad, BIOS/SDK HLE, rendering, audio, input, configuration,
  diagnostics, and platform seams.
- Image-scoped native overrides, scoped original calls, bounded execution exits, and executable-code
  invalidation.
- Hermetic production-seam tests and separately built oracle tools.

The game repository provides exact disc/executable identity, resident/module descriptions, native
functions, title frame/task policy, presentation capabilities, user-facing setup, and legally obtained
game data. Framework code contains no title address or header.

## Initial evidence

1. Authenticate the exact disc and primary executable from headers, filesystem metadata, checksums,
   and known title/revision identity.
2. Recover the executable load address, entry PC, initial stack/global pointer, resident executable
   ranges, and module/overlay load behavior from the binary and runtime observation.
3. Record address reuse by module identity and determine every path that writes executable RAM: CPU,
   DMA, disc loader, decompressor, debugger, and savestate.
4. Establish an independent emulator/hardware or test-only interpreter checkpoint with a positive
   reachability signal and a deliberately differing control.

Static analysis may produce symbols and reviewable metadata. It must not emit guest function bodies or
a title-specific source/object corpus. Ghidra is a maintainer tool, not a player prerequisite.

## Title seam

Define the smallest typed title owner for:

- authenticated resident and module identities;
- module generation/load notifications;
- native override registrations keyed by identity/generation/address;
- frame and cooperative-task exit policy;
- measured BIOS/SDK or device service boundaries;
- renderer/presentation capabilities; and
- representative gameplay checkpoints and legitimate oracle exclusions.

Never expose a bag of arbitrary addresses to the framework or add one virtual getter per address.
Group facts by the algorithm that consumes them. An unknown fact remains absent and fails by name;
zero or a guessed address is not a placeholder.

## Bring-up sequence

1. Load and authenticate the user-supplied executable/module bytes into canonical guest memory.
2. Construct one Lightrec executor for the title's `Core` and execute a bounded resident instruction
   window with nonzero translated blocks.
3. Prove state synchronization and one service exit against independent evidence.
4. Register one resident native override and exercise normal plus scoped original calls.
5. Load two modules that reuse an address and prove identity/generation selects the right native or
   guest body.
6. Exercise executable invalidation with a changed overlapping write and a controlled non-overlap.
7. Reach boot, menu, and gameplay checkpoints while adding only evidence-backed service/native owners.
8. Drive representative interactive gameplay with rendering, audio, input, timing, interrupts,
   module loads, overrides, original calls, and nonzero Lightrec translation active.

Do not repair a missing MIPS semantic with a title-address override. Fix Lightrec or its psxport
integration. Native overrides are for deliberately owned game behavior and proven service boundaries.

## Product and oracle separation

The gameplay build has no CPU engine selector. Lightrec/native execution is always the default. A
missing or unsupported dynarec backend fails by name. After successful initialization, automatic
interpreter fallback may occur only when translation explicitly refuses a classified compilation
failure, unsafe instruction fetch, rare unsupported block, or cache exhaustion. Each fallback call
and instruction is counted, bounded per call and by total guest share, and exceeding either threshold
returns a typed fault.
An explicit interpreter mode or software-GPU oracle is test-only and cannot be selected by product
config, command line, or UI.

The oracle may share canonical state/memory interfaces, but comparison code never becomes a required
player dependency. A consumer's `run.sh` launches the intended Lightrec/native product and never runs
tests.

## Player setup

Support direct game files and one bounded nested ZIP through the platform setup flow. Validate exact
title identity and the complete required install before replacing a previous valid selection. Persist
the choice in the OS user-data location. Developer overrides may use an explicit argument,
environment/`.env`, then a repo drop-in, but no game data enters Git or a package.

A fresh checkout with documented native dependencies, `uv`, and a compatible compiler must provision
and launch without generated guest code, Ghidra, or private machine paths.

## Completion gate

A title is migrated only when:

- the authenticated product reaches at least its prior verified frontier and representative
  interactive gameplay;
- native overrides and an original call execute through the runtime dispatcher;
- relevant module reuse and executable invalidation pass positive and negative controls;
- timing, interrupts, memory, and relevant device state are compared at named boundaries;
- each released host architecture meets its declared correctness and frame-time budget;
- product-link and selector audits find no standalone interpreter mode or generated guest bodies,
  and fallback counters remain below the declared threshold; and
- the launcher, goals, state, codemap, issues, and player docs match the shipped path.

Finish this title before beginning title-specific work for another PSX game.
