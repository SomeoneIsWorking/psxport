---
id: 31
title: Guest VSync can own or query product frame time
status: resolved
symptom: A port can enter Sony libetc VSync, spin to "VSync: timeout", or successfully advance/query display fields outside its native frame loop
tags: timing,vsync,frame-loop,ownership,platform-hle
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The framework had two frame owners. `native_boot.cpp` provided a host loop, but product runtimes could
omit `FrameDriver`, dispatch a non-returning guest main from `bootInit`, and drive presentation through
guest VSync. `Timing::vsync` and the direct-runtime `Timing::vsyncHle` were successful wait/query paths;
`PlatformHlePlan` had no mandatory typed VSync address, and `PlatformHle::register_` allowed the trap to
be replaced. Crash Bash exposed the failure as `VSync: timeout`, but the timeout was the symptom: the
guest had reached a primitive that the product architecture must never call.

## Resolution

`FrameLoopShell::prepareProduct` now runs after title override registration, requires
`Game::frameDriver`, reinstalls and requires the fatal VSync trap, and is the one product preflight used
by both `dc_boot_init` and `native_boot_run`. This order matters because title registration writes the
same native override registry. The shell then delegates exactly one finite frame; `dc_step_frame` and the standalone loop use the same route. The title driver owns its
measured pad/audio/simulation/render/present order and one presentation commit. The framework's former
title-shaped frame body was deleted; a static ownership test rejects its defining operations in
`native_boot.cpp`.

Every direct product supplies `PlatformHlePlan::vsyncAddress`; the adapter supplies the equivalent
measured address. `PlatformHle::initBuiltins` binds it to one framework-private fatal handler that
never reads `GameConfig`, handles modes -1/0/1/N identically, and cannot be replaced through the HLE
table. Missing drivers, missing addresses, refused windows, and conflicting/replacement registrations
abort before guest execution can turn the ownership error into a timeout. The successful
`Timing::vsync` and `Timing::vsyncHle` paths were deleted; host presentation remains the sole shipping
display-field advancement path.

`test_frame_loop_shell` covers exactly-once delegation, missing-driver refusal, and the bad ordering in
which title registration displaces VSync before the product preflight reinstalls the fatal handler.
`test_vsync_ownership` covers direct and adapter initialization, all mode classes, repeated direct
initialization without a title override installed, replacement refusal, missing-address refusal, and
out-of-window refusal. `test_hsync_counter` now covers root-counter MMIO only.
