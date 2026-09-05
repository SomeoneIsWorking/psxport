# Project state

This inventory describes the current psxport tree. Product intent is in `project-goals.md`; remaining
runtime work is specified in `migration.md`.

## Comparison baseline

The baseline is the previous native PSX host framework with no usable product CPU executor. The tree
now has a Linux x86-64 Lightrec execution path; the intended complete delta remains a dynarec-default
Lightrec/native hybrid that executes authenticated user images at runtime and retains the existing
native device, rendering, audio, input, HLE, and enhancement owners.

## Current focus

S013 is the current focus: complete native-service interception and bounded exit handling inside
translated control flow without restoring an offline dispatch substrate.

## Capability inventory

| ID | Capability | State | Dependencies | Goals |
| --- | --- | --- | --- | --- |
| S012 | Per-`Core` dynarec-default Lightrec backend | partial | — | G001, G004 |
| S013 | Explicit architectural state and bounded exits | partial | S012 | G001, G003 |
| S014 | Image/generation-scoped native and original calls | partial | S012, S013 | G001, G002, G003 |
| S015 | Central executable-code invalidation | partial | S012 | G001, G003 |
| S016 | Product path excludes offline guest generation and unbounded interpreter modes | verified | — | G001, G004 |
| S017 | Reusable native PSX platform services | partial | — | G002 |
| S018 | Independent test oracle | partial | S013 | G003 |
| S019 | Narrow multi-title framework seam | partial | S012, S017 | G002 |
| S020 | Native rendering and title-owned enhancements | partial | S017 | G003 |
| S021 | Portable desktop and Android delivery | missing | S012, S019, S020 | G003, G004 |
| S022 | Mechanical structure/config/logging/tooling policy | partial | — | G002, G004 |

### S012 — Per-`Core` dynarec-default Lightrec backend

The maintained fork is pinned at `b1457137c31cedff5f440d59da29401d021ba2da`; Linux x86-64 executes
a nonzero translated block. The shipping executor reports calls, executed blocks/instructions,
fallback blocks/instructions, refusals, and all five admitted/refused reason counts, while the typed
configurable per-call threshold admits the verified single difficult-block escape and refuses a
zero-limit block before any interpreter instruction executes. Gap: GNU Lightning's process-wide
lifecycle currently permits only one initialized
machine, and both AArch64 hosts remain refused.

### S013 — Explicit architectural state and bounded exits

The production adapter transfers GPR, HI/LO, PC, CP0, and COP2 state, accounts measured Lightrec
instruction deltas through the canonical title clock, services pending work at translated block
boundaries, and exposes typed budget/fault/syscall/break/pending-work results. `execute` retains
individual syscall checkpoints; bounded function calls and `dispatchGuestUntilExit` resume supported
syscalls and serviced pending work, with native/HLE routing independent of the optional return
address. A separate typed host-dispatch allowance bounds native-only loops without fabricating guest
cycles. Unknown syscall selectors and delay-slot exceptions are refused before native syscall state
mutation. The synthetic runtime tests cover these paths; complete delay-slot exception continuation
and interrupt conformance still require integrated proof.

### S014 — Image/generation-scoped native and original calls

`runtime/cpu/image_identity.*`, `native_dispatch.*`, and `guest_call.*` provide the top-level API.
The synthetic runtime contract proves translated call interception, nested native context, translated
resume, and an original guest body stopping at the exact caller continuation. Gap: representative
resident/address-colliding overlay and generation-reuse coverage remains absent.

`callOriginalUntilExit` uses the same scoped suppression and native caller-context restoration while
allowing a translated polling body to end at a requested frame/yield/process exit. A synthetic
nested native frame exit proves that suppression and requested-exit state do not leak after return.

### S015 — Central executable-code invalidation

Range normalization, image generations, dispatch revocation, counters, and explicit Lightrec
range/full-cache calls exist. A translated self-modifying RAM store proves a cached block is missed,
retranslated, and changes behavior without fallback; custom store callbacks route exact visible-byte
ranges through this owner. Gap: complete DMA/debugger/savestate mutation coverage still requires
proof.

### S016 — Product path excludes offline guest generation and unbounded interpreter modes

Evidence: the product source/link boundary scan rejects offline guest source, CPU-engine selectors,
and player-selectable interpreter modes; only the backend's classified automatic block fallback
remains.

### S017 — Reusable native PSX platform services

GPU, SPU, GTE, MDEC, CD, DMA, timing, pad, BIOS/HLE, and host-presentation owners remain. Gap:
complete service coverage and integration are title-driven and have not been demonstrated in the
dynarec product path.

### S018 — Independent test oracle

Beetle/Mednafen trace tools remain separate from the product. Gap: comparison through the production
state bridge and a seeded first-divergence proof remain absent.

### S019 — Narrow multi-title framework seam

The shared runtime API includes typed product/compare-candidate/compare-reference roles and owns
comparison-run enhancement suppression without title-local environment parsing. Gap: consumers
still require coordinated pins and representative gameplay requalification without copied
framework code.

### S020 — Native rendering and title-owned enhancements

The framework rendering and presentation owners remain. Gap: native rendering and each enhancement
must be requalified with the dynarec product path.

### S021 — Portable desktop and Android delivery

Missing capability: no gameplay-ready backend, Windows host contract, macOS arm64 evidence, Android
`arm64-v8a` evidence, or release package exists. Hosted CI therefore runs only the proven Linux
x86-64 synthetic runtime; unsupported hosts have no green placeholder jobs.

### S022 — Mechanical structure/config/logging/tooling policy

Canonical Python build/verification tooling runs through the checked-in `pyproject.toml`/`uv.lock`;
a shared declarative consumer verifier, pinned full-history Linux CI with exact Lightrec and
maintained-Lightning inputs, product-boundary checks, and seeded whole-tree architecture,
deleted-path, and current-runtime policy checks exist. Gap: remaining large runtime owners still
need responsibility-driven extraction.

Nested CMake tests receive the parent configure's exact Lightning library/include paths and compiler
inputs, use stable directories under `build/test-fixtures`, and preserve the locked Python executable
symlink. Submodule-sync fixtures supply their own Git identity for every command. These repair the
three fixture failures in hosted run `33893969146`; a subsequent hosted run must establish CI success.

## Blocking dependency

The direct maintained Lightrec fork is now the Linux x86-64 product dependency. Hosted verification
builds the maintained GNU Lightning fork at exact revision
`f305e50483e9794e8fb0b99fb77d82954a4924e5` and passes its validated installed prefix explicitly to
Lightrec instead of relying on a distribution package or ambient sibling checkout. Gameplay is still
blocked on multi-`Core` lifecycle support, complete executable-write invalidation coverage, and
representative title/device integration. Its AArch64 dependency also
needs Android x18 reservation and macOS `MAP_JIT`/per-thread write-protection coverage for allocator
metadata, code, and alternate buffers.

## Host evidence gaps

- x86-64 Linux: real translated/native/original-call/resume control flow, architectural register
  transfer, exact return stopping, measured timing, pending work, self-modifying-code retranslation,
  typed unsafe-fetch fault, zero/nonzero per-reason fallback telemetry, and pre-execution threshold
  admission/refusal behavior pass in `test_dynarec_contract`; typed comparison enhancement suppression passes in
  `test_diagnostic_run`; representative gameplay is absent.
- Apple Silicon macOS: no build, executable-memory, instruction-cache, ABI, or device evidence.
- Android `arm64-v8a`: no NDK build, executable-memory, instruction-cache, ABI, APK, or device evidence.
- Windows: not a declared supported host because neither the maintained Lightrec dependency nor
  psxport has a Windows build/runtime contract; no CI or delivery claim is made.

No title is migrated until it runs representative interactive gameplay with nonzero translated
blocks, native/original calls, invalidation, devices, rendering, audio, and input on the intended
product path.
