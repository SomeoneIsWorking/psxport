# Native-call framework during the Lightrec migration

The product porting contract is `docs/porting-a-new-psx-game.md`. This document exists because current
analysis tools still reference it while generated-body consumers are being removed; it defines only
how to migrate those callers, not a second port methodology.

## Replace implementation identity with runtime identity

Current title code may name an emitted host function or wrapper. Do not preserve that shape through a
compatibility wrapper. Recover the original guest address and complete resident/module identity, then
replace the call with one production runtime operation:

- normal dispatch, which honors the current image-generation-plus-address override; or
- scoped original dispatch, which bypasses only the current override and executes the guest body
  through Lightrec.

The framework API owns CPU state transfer, bounded exits, nested calls, and failure reporting. Callers
do not open-code GPR moves, host-stack guest frames, override-disable booleans, or engine selection.

## Native body requirements

A native body is accepted only when reverse engineering names its behavior and independent evidence
proves its guest-visible contract. Use precise types, explicit dependencies, and the title's cohesive
owner. A native body may intentionally replace an original algorithm, but it must preserve required
state, callbacks, resources, and lifecycle transitions. Missing MIPS semantics are fixed in Lightrec or
the state/device integration, never with a title-address override.

## Existing migration tools

`tools/port_gen.py`, `tools/port_check.py`, and `tools/abi_extract.py` inspect the current generated-C
callers. They are not product generators or completion gates for the Lightrec architecture. Use them
only to inventory/remove existing dependencies, confirm every retained fact against the original
binary/runtime evidence, and delete them with the offline pipeline after consumers migrate.

## Verification

Drive the production Lightrec/native dispatcher. Prove the body ran, prove the original-call path ran,
and compare guest registers, memory, cycles, interrupts, and relevant device state at a named bounded
exit. For overlay code, test a same-address different-generation case. The negative control must show
the instrument detects a wrong store/register/dispatch answer.
