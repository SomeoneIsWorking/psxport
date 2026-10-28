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

Every block boundary the executor reaches is classified (guest code, host service, unclaimed) through
`NativeDispatcher::classify`. With block linking off in Lightrec the callback runs at every block entry, so its fast
path is a header-inline lookup: the verdict slot for the address, valid while its stored `DispatchEpoch` equals the
Core's. The epoch is one counter its input owners advance where they mutate (`ImageCatalog` activation, deactivation
and subtraction, override install and removal, `PlatformHle` registration, the Game binding), so a device write that
retires an image inside a segment is seen at the next block. A suppression (`callOriginal`) changes only its own
address's verdict and drops that slot alone. A verdict that also reads live guest memory (the BIOS pad work-area
entries) is stored with `live` and re-derived at every crossing; the check for it runs once per address per epoch.
The Lightrec callbacks reach the boundary context of the `executeWithBoundary` whose `lightrec_execute` is running
through one pointer set around that call: the callbacks never re-enter `lightrec_execute`, and fibers switch only in
host services between segments.

Cost, Tekken 3's fight after `warp 0 1` (`perf record -e instructions:u`): the boundary path was about 27% of the
process's instructions (`blockBoundary` 10.5%, `classify` 6.8%, the epoch compare 2.3%, the thread lookup 1.9%,
the revision reads 2.1%, the slot lookup 1.2%, `resolveHostDispatch` and `intercepts` 2.2%) and is about 22% after (`blockBoundary` 17.6% with
the classify fast path inlined; `resolveHostDispatch` and `intercepts` 2.3%, which every host-service dispatch
also reaches). The rest is the callback itself (writer latch, pending-work
checks, register-file access); removing the per-block callback needs block linking in the Lightrec fork.

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
