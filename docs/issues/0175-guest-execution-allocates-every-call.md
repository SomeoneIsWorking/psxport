# 0175: guest execution allocates on every native and host-service call

Status: open. Owner: `runtime/cpu/execution_exit.h` `ExecutionResult`, `runtime/cpu/native_dispatch.cpp`,
`runtime/cpu/side_effect_journal.*`, `runtime/cpu/guest_call_census.*`, `runtime/hle` `returnFromException`.

## Observed
Found by psx-spyro 0196 pass 6 after 0173's fix: malloc calls on the main thread over a 10 s gdb window of Spyro 1
`warp 10` gameplay (`scratch/judder-pass6/alloc-s1b`) and Spyro 2 `warp 21` (`alloc-s2`). Producers, the composer,
record sealing, the OT walk and (since the 0173 review fix) the window allocate nothing in steady state; what
remains is guest execution, plus host work listed after the table:

| site | Spyro 1 | Spyro 2 |
|---|---|---|
| `completeHostLeaf` builds `ExecutionResult::detail` from the native's name | 7248 | 1821 |
| `dispatchGuestHostService` `ExecutionResult` detail | 7248 | 2185 |
| `SideEffectJournal::append` | 0 | 546 |
| `GuestCallCensus::recordCompleted` | 360 | 549 |
| `__cxa_allocate_exception` in `hle::returnFromException` (an exception per BIOS exception return) | 240 | 365 |

Host side, `fix-alloc-s1` (build `7b6f31f+psxport-de34a6cf`, 10 s, load 1.16-1.20): the Vulkan driver's own
submit and swapchain acquire (`radv_queue_submit`, `vk_queue_submit_*`, `wsi_create_sync_for_image_syncobj`, about
5000 in all, outside our code), Lightrec compiling cold blocks (`jit_alloc`, `lightrec_malloc`, about 1000), and
`GpuPerf::presentSummary` formatting its string every 600 presents (1), which it builds before the `playcheck`
debug channel is checked.

## Proper fix
`ExecutionResult::detail` a `std::string_view` over a static or registered name (or an enum), the census and journal
keeping fixed or reused storage, and the HLE exception return a bounded executor exit rather than a C++ throw. `GpuPerf` formats its summary
only when the channel is on.
