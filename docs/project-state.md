# Project state

## Comparison baseline

The baseline is implementing each PlayStation port as a one-off emulator-derived runtime or running
the original title wholly inside a PS1 emulator. psxport instead provides a reusable fail-closed
MIPS-to-C substrate, native console-service owners, differential comparison, and title-owned seams for
native rendering, widescreen, and interpolation.

## Current focus

S004 is the current focus.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | MIPS R3000A executables are translated into deterministic generated C | verified | — | G001 |
| S002 | Native runtime owners provide GPU, SPU, GTE, MDEC, CD, XA/FMV, and BIOS services | partial | S001 | G001 |
| S003 | Shipping differential infrastructure compares independent guest-state snapshots and frame boundaries | verified | S001 | G001 |
| S004 | Game-owned native producers replace guest graphics while supporting widescreen and interpolation | partial | S002 | G001 |
| S005 | Multiple title repositories consume the framework through narrow title-owned seams | verified | S001, S002 | G001 |
| S006 | The native GPU owner supports PSX drawing plus title-owned widescreen presentation | partial | S001 | G001 |
| S007 | The native SPU owner produces game sound without a PS1 emulator process | partial | S001 | G001 |
| S008 | The native GTE owner provides geometry and lighting operations used by translated games | partial | S001 | G001 |
| S009 | Native MDEC and FMV owners decode and present PlayStation movies | partial | S001 | G001 |
| S010 | Native CD and XA owners provide game data and streamed audio from user-supplied media | partial | S001 | G001 |
| S011 | Native BIOS and SDK services replace the console firmware calls exercised by ports | partial | S001 | G001 |

## Capability details

### S001 — Static translation

Evidence: the shared build path emits C shards from recovered executable functions and is consumed by
multiple maintained PlayStation port repositories.

### S002 — Console service replacement

The framework has production GPU, SPU, GTE, MDEC, CD, XA/FMV, BIOS/SDK, and CHD-backed service owners.

Gap: service and title coverage remains incremental; unsupported semantics must still be grounded and
implemented as new titles reach them.

### S003 — Differential verification

Evidence: the shipping `ndiff_run` path retains independent complete snapshots and focused tests cover
nested snapshots, modeled returns, frame contracts, and negative controls.

### S004 — Native presentation

The framework exposes native scene producers, depth-aware rendering, interpolation, and title-owned
picture policy used by live ports.

Gap: each title still needs complete game-state producers; guest post-projection packets are not an
acceptable fallback.

### S005 — Multi-title consumption

Evidence: Tomba, Crash, Crash Bash, Mega Man X4, Spider-Man, Spyro, Tekken 3, Toy Story 2, and Vagrant
Story repositories consume the same framework while retaining title-specific policy.

### S006 — Native GPU

The production GPU owner supports the retained PSX drawing path and title-owned native scene
producers, depth-aware rendering, widescreen, and interpolation.

Gap: drawing and title coverage remains incremental across consumers.

### S007 — Native SPU

The production runtime owns SPU execution and audio output.

Gap: complete audio behavior is established title by title rather than for every PSX program.

### S008 — Native GTE

The production runtime implements the geometry operations exercised by maintained consumers.

Gap: unsupported semantics still require evidence-driven implementation as new titles reach them.

### S009 — Native MDEC and movies

MDEC and FMV owners are integrated into the native runtime.

Gap: codec, timing, and title coverage remains incomplete outside measured paths.

### S010 — Native CD and XA

CHD-backed CD access and XA streaming owners are integrated into the native runtime.

Gap: complete drive and streaming behavior remains incremental across titles.

### S011 — Native BIOS and SDK services

The runtime replaces measured BIOS and SDK calls with native services.

Gap: the service surface is extended fail-closed as maintained titles exercise new semantics.
