# Faithful execution boundaries

Faithful execution is the observable PSX contract the native/Lightrec product preserves before
intentional enhancements.

## One architectural state

`Core` owns the framework-visible PSX state. The state bridge transfers that complete boundary into
Lightrec on entry and back on every host-visible exit. GPRs, HI/LO, PC/delay state, CP0, GTE,
interrupts, and cycles are never read from two competing authoritative copies.

Native functions receive typed access to `Core` state through the normal ABI owner. They do not
open-code register shuffles at call sites or preserve host-stack assumptions from an offline
translation. When native code invokes guest behavior, it calls the executor by guest identity/address:
normal dispatch honors overrides; original dispatch suppresses only its current override and executes
the guest body through Lightrec.

## Explicit suspension and return

Guest execution is always bounded. Lightrec returns a typed reason for budget, native/HLE/device,
interrupt/exception, frame/VSync, thread yield/exit, or fault. Host code commits and handles that
state, then resumes deliberately. It never unwinds a C++ exception through JIT frames or uses a host
coroutine stack as the owner of guest continuation state.

A skip or synchronous native service establishes the complete guest-visible lifecycle invariant. It
does not fast-forward simulation, write a state-machine phase/timer/scene pointer, or omit required
callbacks and resource transitions.

## Image identity

Resident code and loaded modules can reuse an address. Every dispatch, continuation, override, and
invalidation decision therefore carries authenticated image/module identity and load generation as
well as the address. Loading a replacement generation makes stale continuations and override keys
unusable even if the bytes or range appear identical.

## Verification

Use an independent emulator/hardware trace, binary evidence, or the separately built test interpreter
to diagnose the first divergence. Tests drive the production state bridge, executor exits, dispatch,
and invalidation; they do not reimplement those rules beside the product. Each instrument demonstrates
both a match and a deliberately seeded mismatch and reports how many blocks, exits, ranges, or state
fields it examined.

Representative interactive gameplay is the completion bar. Boot, logos, menus, FMV, isolated leaf
calls, and a zero-diff frame are checkpoints only.
