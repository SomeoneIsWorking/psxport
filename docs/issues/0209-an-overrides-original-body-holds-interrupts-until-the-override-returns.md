# 0209 — An override's original body holds interrupts until the override returns

Status: fixed (2026-11-18). Owner: `runtime/cpu/native_dispatch.cpp` (`NativeExecutionScope`, `executeOriginal`), with
`runtime/cpu/guest_executor.cpp` (`pendingWorkDeliverable`).

## Observed
CTR, `arcade-crash-cove-drive.pad` at 4:3: binding an override on overlay 226's `DrawLevelOvr1P` (0x800A0CBC) that
only calls the original moves `msInThisLEV` (gGT+0x1D00) from 108951 to 108950 at field 6,000. A per-field log of
the frame's `dt` (gGT+0x1D04) differs in one field only: field 2,849 has dt 31 with the override and 32 without.

## Cause
`NativeExecutionScope` sets `core.active_native_address` for the whole override, and `executeOriginal` runs the
original body under it, so `pendingWorkDeliverable` refuses pending work at every block entry of that body. An
interrupt raised during the original is taken only after the override returns. CTR's vertical-blank callback
(`FUN_80034AA4`) samples root counter 1 for the frame's dt, so a vblank landing inside the level draw is sampled late.
Every override that calls a long original carries the same deferral.

## Proper fix
Deliver pending work at block entries of an original body called from an override (`executeOriginal`,
`resumeOriginal`, `resumeGuestToReturn`), as an ordinary guest call would, with the nested run servicing it rather
than exiting with `HostService`. It changes interrupt timing in every title that has such overrides, so it is
checked in real time on Tomba 2 and Spyro.

## Fix
`OriginalBodyScope` (`executeOriginal`, `resumeOriginal`) marks the override whose original body runs
(`Core::native_original_address`); `Core::inNativeHostCode` is true only while an override's own host code runs, and
`pendingWorkDeliverable` and `Hle::canDispatchInterrupt` test it, so interrupts and pending work reach an original body
at its block entries as on hardware and still wait in host code.

## Proof
Same replay to field 6,000 with the override bound: `msInThisLEV` 108951, race clock 99826 and all eight drivers equal
to the build without the override. Tomba! 2 (`warp 0/0`) and Spyro 1's demo run with faults 0 and no refused
in-betweens, their pictures unchanged.
