# Native-call framework

The complete consumer contract is in `docs/porting-a-new-psx-game.md`. Native calls use the active
authenticated image identity, load generation, and guest address; an address alone never identifies
overlay code.

## Dispatch

A normal call honors the active native override for the complete runtime identity. A scoped original
call bypasses only that exact override for one dynamic call and executes the guest body through the
dynarec. Nested calls continue to honor every other override.

The framework owns CPU-state synchronization, bounded exits, nested-call attribution, and typed
failure propagation. Callers do not open-code register marshalling, guest frames, override-disable
flags, cache invalidation, or execution-backend selection.

## Native body requirements

A native body is accepted only when reverse engineering names its behavior and independent evidence
proves its guest-visible contract. Use precise types, explicit dependencies, and the title's cohesive
owner. An intentional replacement must preserve required state, callbacks, resources, and lifecycle
transitions. Missing MIPS semantics belong in Lightrec or the PSX integration, never in a
title-address override.

## Verification

Drive the production Lightrec/native dispatcher. Prove both the override and scoped-original paths,
then compare registers, memory, cycles, interrupts, and relevant device state at a named bounded
exit. For overlay code, test equal addresses under different image generations. A negative control
must demonstrate that the instrument detects an incorrect store, register, or dispatch result.
