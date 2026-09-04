# Project goals

This document owns psxport's epic product intent. Factual capability state lives in
`docs/project-state.md`; ownership and placement live in `docs/codemap.md`; implementation order and
acceptance gates live in `docs/migration.md`; atomic work lives in `docs/issues/`.

## G001 — Native/dynarec guest execution

Provide one game-agnostic PSX executor in which verified native overrides cooperate with a maintained,
pinned Lightrec dynarec for every remaining MIPS R3000A instruction.

Why it matters: a port should consume the user's original game image at runtime and behave as a
maintainable native application without generating and compiling a title-sized source corpus.

Success conditions:

- One Lightrec instance is owned by each live `Core`, with no implicit process-global CPU, image,
  override, clock, or invalidation state.
- Gameplay objects and links contain no interpreter, generated guest bodies, static dispatcher, or
  engine-selection/fallback surface.
- Native overrides use complete image/module-generation-plus-address identity. A scoped original call
  bypasses only its current override and executes the guest body through Lightrec without recursion.
- Lightrec owns its block cache and executable memory. psxport owns explicit architectural-state
  synchronization, bounded exits, callbacks, native dispatch, and invalidation.
- CPU writes, DMA, module loads, debugger writes, and savestate restore invalidate every affected
  translated block and stale dispatch decision.

Non-goals: designing a new MIPS JIT, choosing between an interpreter and Lightrec, wrapping Lightrec's
cache in `jit-common`, or retaining offline guest-code generation as a compatibility mode.

Contributing state items: S012–S016.

## G002 — Reusable native PlayStation platform

Provide cohesive, title-neutral owners for the PSX services games exercise: GPU, SPU, GTE, MDEC, CD,
DMA, timers, pads, BIOS/SDK HLE, rendering, audio, configuration, diagnostics, and platform lifecycle.

Why it matters: title repositories should contain game identity, recovered game behavior, and product
policy rather than divergent copies of console and host infrastructure.

Success conditions:

- psxport includes no title headers, addresses, or behavior; game facts cross narrow typed seams.
- Each service has one owner with explicit per-game/per-core lifetime, state, and error contract.
- Native functions call guest code only through the runtime executor's normal/original-call APIs.
- Multiple title repositories consume the same pinned framework without copied framework code.

Non-goals: moving title gameplay logic into psxport or forcing unrelated services into one framework
class.

Contributing state items: S017, S019, S022.

## G003 — Faithful, representative conformance

Establish that the native/Lightrec product works through representative interactive gameplay, not
merely a boot logo, menu, attract loop, FMV, or clean internal trace.

Why it matters: early checkpoints do not exercise the override, overlay, timing, input, rendering,
audio, and device interactions that determine whether a game is actually playable.

Success conditions:

- A bounded reference-consumer scenario exercises resident and address-colliding overlay code,
  native overrides, an original call, interrupts/timing, input, rendering, and audio.
- Independent emulator, hardware, binary, or separately built test-oracle evidence can stop at the
  first divergence and reports reachability and denominators.
- Every released host architecture demonstrates nonzero Lightrec execution, invalidation, correct
  state boundaries, and an interactive frame-time/correctness budget.
- Enhancements such as widescreen and interpolation remain explicit intentional divergences layered
  on a faithful baseline.

Non-goals: pixel-perfect matching as a product gate or treating the old generated-C product as a
permanent oracle.

Contributing state items: S013–S015, S018–S021.

## G004 — Portable, maintainable framework delivery

Make the framework and its consumers build from a fresh checkout with normal native dependencies and
user-supplied game files, while keeping code ownership, configuration, logging, and verification
mechanically enforceable.

Why it matters: the intended product must not rely on a maintainer's Ghidra project, generated corpus,
machine paths, or undocumented runtime flags.

Success conditions:

- Lightrec is a direct dependency at an immutable maintained revision with recorded provenance; any
  required changes live as commits in a maintained fork, never patch files.
- Consumer launchers authenticate user game files and start the native/Lightrec product without an
  offline translator, generated corpus, or engine flag.
- One typed configuration owner and Lucent logging boundary are enforced; product modules do not read
  environment/config files or print diagnostics directly.
- Structure, format, lint, hermetic tests, product-link audits, and representative gameplay checks
  fail with named causes and non-empty denominators.

Non-goals: requiring Ghidra for players, shipping copyrighted game data, or encoding the maintainer's
Clang choice as a user-facing compiler restriction.

Contributing state items: S016, S021, S022.
