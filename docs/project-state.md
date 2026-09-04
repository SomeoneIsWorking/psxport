# Project state

This is the factual capability inventory for psxport. It does not claim that the plans in
`docs/migration.md` are implemented.

## Comparison baseline

The comparison baseline is the current psxport workflow: game executables and overlays are translated
offline into generated C, compiled into each consumer, and selected alongside an interpreter through
runtime execution-engine policy. The intended product instead authenticates the user's original image
and executes every non-native guest path on demand through Lightrec, with any interpreter confined to
a separate test target.

## Current focus

S012 is the current focus: establish the per-`Core`, dynarec-only Lightrec product executor before
extending other execution paths.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S012 | A maintained pinned Lightrec dynarec executes product guest code per `Core` | missing | — | G001, G004 |
| S013 | Architectural state crosses Lightrec/host boundaries explicitly and execution exits are bounded | missing | S012 | G001, G003 |
| S014 | Native overrides and original calls are scoped by image/module generation and address | missing | S012, S013 | G001, G003 |
| S015 | Executable-memory changes and override policy invalidate every affected Lightrec path | missing | S012, S014 | G001, G003 |
| S016 | Gameplay builds contain no interpreter, generated guest corpus, engine selector, or fallback | missing | S012, S013, S014, S015 | G001, G004 |
| S017 | Native PSX platform services are reusable across title repositories | partial | — | G002 |
| S018 | Independent comparison and test-only oracle surfaces can diagnose guest-state divergence | partial | S013 | G003 |
| S019 | Multiple titles consume one game-agnostic framework through narrow typed seams | partial | S014, S017 | G002 |
| S020 | Native rendering supports PSX presentation and title-owned enhancements | partial | S017 | G003 |
| S021 | Consumer delivery provisions user assets and launches the intended product without maintainer-only tools | partial | S016, S019 | G003, G004 |
| S022 | Structure, configuration, logging, and verification boundaries are mechanically enforced | partial | — | G002, G004 |

## Capability details

### S012 — Per-Core Lightrec product executor

Missing capability: add a direct dependency on an exact maintained Lightrec revision and construct one
dynarec-only Lightrec state for every live `Core`. The product integration must use Lightrec's own
block cache and executable-memory owner and must not depend on the indirectly bundled Beetle copy.

Related issue: 0047.

### S013 — State synchronization and bounded exits

Missing capability: define and implement one explicit bridge for GPRs, HI/LO, PC/delay state, CP0,
GTE, pending interrupts, and cycles, plus typed budget/native/HLE/interrupt/frame/thread/fault exits
that never unwind C++ exceptions through JIT frames.

Related issue: 0048.

### S014 — Image-scoped native and original calls

Missing capability: replace generated-symbol and address-only dispatch with a per-`Core` runtime key
containing authenticated image/module identity, load generation, and guest address. A scoped original
call must bypass only the current override while nested calls retain normal dispatch.

Related issue: 0049. The cross-portfolio audit measured 816 unresolved generated-body/wrapper symbols
across 109 Tomba! 2 game files when its generated corpus was removed; this is consumer-boundary
evidence, not a psxport implementation claim.

### S015 — Runtime invalidation

Missing capability: route CPU stores, DMA, module loads, decompression, debugger writes, savestate
restore, and override changes through one normalized executable-range invalidation owner that reaches
Lightrec and revokes stale image-generation/dispatch decisions.

Related issue: 0050.

### S016 — Product-path isolation

Missing capability: remove the gameplay link and selection paths for the offline translator,
generated dispatch, psxport interpreter, and Lightrec interpreter fallback. A separately built test
oracle may remain, and product-link/selector inspection must prove the isolation.

Related issues: 0047 and 0051.

### S017 — Native PSX services

The repository contains title-neutral GPU, SPU, GTE, MDEC, CD, DMA, timing, pad, BIOS/SDK HLE, CHD,
audio, and host-rendering owners used by maintained consumers.

Gap: service coverage is established incrementally by reached title paths, and several owners still
live in the broad `runtime/recomp/` directory or retain legacy game-configuration projections.

### S018 — Independent diagnostics and test oracle

The repository has an independent Beetle/Mednafen-oriented oracle boundary, exact-PC/call-boundary
trace support, and an in-process interpreter used for focused state comparison.

Gap: the interpreter is still compiled into the product-facing framework and the comparison surfaces
are coupled to static/generated execution assumptions. They must move to separately built test-only
targets and compare against the Lightrec state boundary without entering the gameplay link.

### S019 — Multi-title framework consumption

Tomba, Crash, Crash Bash, Mega Man X4, Spider-Man, Spyro, Tekken 3, Toy Story 2, and Vagrant Story
repositories consume the same framework and retain title-specific policy.

Gap: their current call seams still include generated-body symbols and legacy configuration/override
paths. The Lightrec executor and image-scoped native-call API do not exist yet.

### S020 — Native presentation

The framework exposes retained PSX rendering, native scene producers, depth-aware rendering,
widescreen projection support, interpolation infrastructure, and title-owned guest-VRAM picture
policy.

Gap: drawing and title coverage remain incremental, and representative gameplay must be requalified
with Lightrec as the product executor before the migration can claim preservation.

### S021 — Portable consumer delivery

Consumers already resolve user-supplied disc images and provide player launchers; framework binaries,
restricted inputs, and diagnostics are kept outside tracked source.

Gap: current launch/build paths still provision generated guest C and may expose execution-mode flags.
A fresh checkout cannot yet launch the intended Lightrec/native product without the offline pipeline.

### S022 — Engineering boundaries

The repository has tracked Clang formatting/tidy configuration, a normal style/structure gate, a CVar
system, Lucent integration, hermetic framework tests, and a game-agnostic smoke target.

Gap: direct environment reads, legacy logger call sites, broad runtime modules, engine-selection
policy, and generated-code tests remain. The normal verifier does not yet audit that gameplay links
contain neither interpreter nor generated guest bodies.
