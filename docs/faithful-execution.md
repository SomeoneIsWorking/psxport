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

### Resuming, which is the whole point of a bounded exit

`ExecutionBudget::currentTurn` is one display field — `33'868'800/60 = 564,480` cycles — by
construction, so exceeding it is routine rather than exceptional. The two entry points for continuing
are `psx::cpu::resumeOriginal` and `psx::cpu::resumeGuestToReturn`
(`runtime/cpu/native_dispatch.h`): pass the `ExecutionResult::guestPc` a bounded turn reported, the
return address the call must stop at, and a fresh budget. On `GuestReturn` the call has completed; on
`BudgetExhausted` the new `guestPc` is the next resume point. `resumeOriginal` additionally re-establishes
the native-suppression scope for its `NativeKey`, so an override cannot be re-entered by guest code
running inside the original it is resuming.

**A host that aborts on `BudgetExhausted` is a defect, not caution.** Measured 2026-09-26: two titles
aborted at 564,5xx cycles for calls that were finite compute. Mega Man X4's `DecDCTvlc` terminates on its
own after 610,746 cycles — 1.082 fields — and returns `GuestReturn` at the correct return address
`0x80018AA0`; it was being killed for needing 8% more than a field. Crash Bash's MENU image channel swap
at `0x80018AA0` is a bounded full-image loop whose body is only `lhu`/`sh` on RAM, so it cannot be
blocked at all. The budget constant is deliberately not a tuning knob, so resuming is the only route.

The same investigation found three repositories had each written their own suspend/resume for this one
contract, and a fourth could not compile because the wrappers did not exist. There is now one
implementation. Its hermetic coverage is a real gap and is recorded as one: the only bound the executor
consults between segments is Lightrec's HOST cycle counter, so a cycle-sized budget is
machine-dependent, a loop Lightrec closes over completes in one segment regardless, and a guest loop
calling a host override never reaches a segment boundary. The composition — truncate a call mid-loop,
then resume it — is therefore closed at the product level rather than by a synthetic loop that only
looks like coverage. `executeFunction`'s arbitrary-entry / supplied-return-address behaviour is covered
by `test_explicit_function_continuation_is_independent_of_incoming_ra`.

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
