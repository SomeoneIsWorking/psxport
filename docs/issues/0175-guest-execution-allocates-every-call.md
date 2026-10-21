# 0175: guest execution allocates on every native and host-service call

Status: open. Owner: `runtime/cpu/execution_exit.h` `ExecutionResult`, `runtime/cpu/native_dispatch.cpp`,
`runtime/cpu/side_effect_journal.*`, `runtime/cpu/guest_call_census.*`, `runtime/hle` `returnFromException`.

## Observed
Found by psx-spyro 0196 pass 6 after 0173's fix: malloc calls on the main thread over a 10 s gdb window of Spyro 1
`warp 10` gameplay (`scratch/judder-pass6/alloc-s1b`) and Spyro 2 `warp 21` (`alloc-s2`). Producers, the composer,
record sealing and the OT walk allocate nothing in steady state; what remains is guest execution:

| site | Spyro 1 | Spyro 2 |
|---|---|---|
| `completeHostLeaf` builds `ExecutionResult::detail` from the native's name | 7248 | 1821 |
| `dispatchGuestHostService` `ExecutionResult` detail | 7248 | 2185 |
| `SideEffectJournal::append` | 0 | 546 |
| `GuestCallCensus::recordCompleted` | 360 | 549 |
| `__cxa_allocate_exception` in `hle::returnFromException` (an exception per BIOS exception return) | 240 | 365 |

## Proper fix
`ExecutionResult::detail` a `std::string_view` over a static or registered name (or an enum), the census and journal
keeping fixed or reused storage, and the HLE exception return a bounded executor exit rather than a C++ throw.
