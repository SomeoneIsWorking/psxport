# Test-only oracle architecture

The oracle exists to diagnose the first divergence; it is not a gameplay engine. The production
library always executes non-native guest code through Lightrec. Any interpreter and software-GPU
reference are built into separate test/diagnostic targets that the gameplay library and consumers do
not link and cannot select through configuration, CLI, UI, or fallback.

USER, 2026-07-01: *"We need a real oracle for INTERLEAVED DIVERGENCE COMPARE"*.

That requirement remains: comparison must align meaningful game state, report the first observable
divergence, and state each instrument's blind spot. The old in-product `use_interp`/
`PSXPORT_SBS_MODE=oracle` routing is not the target boundary and is removed by the migration in
`docs/migration.md`.

## Separate targets

- The gameplay `psxport` library contains the Lightrec executor, canonical `Core` state, memory/device
  owners, native dispatch, and no interpreter objects or symbols.
- A test-only interpreter target may reuse the production state types and memory/service interfaces.
  Its CPU state and diagnostic provenance are per test core.
- A test-only software-GPU target may render the oracle core's GP0 stream into independent VRAM.
- A test harness links the production Lightrec library and the test-only oracle libraries, drives both
  with authenticated user data, and compares explicitly declared state at named barriers.

Building the harness is what selects the oracle. No runtime engine selector enters the product config
or UI. Product-link inspection names the objects/symbols examined and proves interpreter absence;
observing zero interpreter executions in one run is insufficient.

## State-aligned comparison

Native services may complete console waits synchronously, so equal presentation-frame numbers need
not represent equal game progress. Comparisons use ordered, title-owned state predicates:

1. Run each core toward the next checkpoint using per-core input policy.
2. Park the first arrival without advancing its guest state.
3. Once both arrive, compare only the declared CPU/game/device state and name every excluded field.
4. Report both cores' frames, cycles, translated/interpreted instructions, checkpoint reachability,
   and first differing byte/register.
5. Resume toward the next checkpoint.

The framework owns the barrier mechanism. The consuming title owns checkpoint predicates, legitimate
exclusions, and representative input. An absolute frame count is not a state predicate.

## Evidence rules

- Validate the comparator with a matching case and a deliberately seeded register/RAM divergence.
- Validate reachability before interpreting the absence of a difference.
- Keep hardware, CPU, state, and pixel questions separate; no single oracle answers all of them.
- A boot, logo, FMV, or non-interactive scripted pose is limited checkpoint evidence, not gameplay
  conformance.
- Preserve exact source/target identity, settings, state fields, exclusions, and denominators in the
  report that uses the result.

## Available evidence and blind spots

| Instrument | What it can answer | Blind spot |
| --- | --- | --- |
| Independent Beetle/Mednafen CPU trace tools under `tools/oracle/` | Instruction/call boundaries and selected CPU/device state against an independent implementation | Not automatically aligned to a native title checkpoint; scope must be declared |
| Test-only flat MIPS interpreter | Guest CPU/RAM behavior on reached paths using psxport state/memory seams | Shares psxport platform models and is not an independent device oracle |
| Test-only software GPU | PSX-style GP0 raster output from the oracle command stream | Does not prove native scene construction or host renderer correctness |
| Guest dispatch tables and no-op arms | Whether original title data selects/submits a behavior or draw | Says nothing about final appearance |
| Native renderer readback | What the current host renderer produced | Cannot establish what original hardware should produce by itself |

## Retained observations

The existing in-process interpreter/software-GPU work established useful, bounded facts that remain
valid inputs to later tests:

- State barriers are required because synchronous native CD/service ownership makes native and
  console-style paths reach the same game state on different presentation frames.
- Tomba! 2 narration and opening-field comparisons converged at their declared checkpoints except for
  named PRNG, callback-ring, stdio, and render-cache/OT differences; those observations were used to
  diagnose renderer ownership rather than certify whole-game parity.
- The software-GPU oracle showed the void scene contains a black fill plus a semi-transparent textured
  swirl, character, and text, while the cliff scene contains the complete sea tiles. Those observations
  corrected native scene-selection behavior.
- A later Tomba! 2 "interactive" scan held input while the character remained in a scripted caught
  pose. It reconfirmed still-state convergence only and explicitly did not prove player-control
  behavior.
- A 2026-08-28 Spyro oracle boot reached 120 fields and exited cleanly while retaining five persistent
  stack-only byte differences and one registered owner never reached. This is limited boot evidence,
  not whole-game equivalence.

These are evidence facts, not justification for keeping interpreter code in the gameplay library.
Implementation moves the still-useful mechanisms to the separate test targets and revalidates them
against the production Lightrec state bridge.
