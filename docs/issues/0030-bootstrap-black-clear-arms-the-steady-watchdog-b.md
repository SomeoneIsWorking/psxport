---
id: 30
title: Bootstrap black clear arms the steady watchdog before guest boot
status: resolved
symptom: Crash Bash completes two module loads and reaches MENU but the no-present watchdog fires on the steady timeout before a real game frame
tags: watchdog,presentation,bootstrap,lifecycle,crashbash
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

`native_boot_run` presents an intentional black transition with `gpu_clear_display` before entering
guest crt0. That helper routed through the ordinary `gpu_present_ex` completion path, whose
unconditional `watchdog_main_present_complete` call permanently changed the watchdog from boot
grace to the steady frame timeout. A transition clear is visible output and forward progress, but it
does not prove that the main game-frame presenter is ready.

The exact Crash Bash consumer log makes the phase error observable: the watchdog announces a
2-second steady timeout and 45-second boot grace, the black present occurs before `entering native
crt0`, and the eventual alarm omits the `before the main presenter became ready` hint. Both measured
module loads and MENU execution occur after that premature transition and before the two-second alarm.

## What was tried / dead ends

Increasing the steady timeout, disabling the watchdog, or treating guest log lines/generated MENU
dispatch as watchdog heartbeats would hide the ownership error. CDC and overlay correctness remain
separate semantic gates; activity is not state agreement.

## Resolution

`GpuPresentCompletion` now makes the lifecycle decision explicit at the shared presentation owner.
Ordinary `gpu_present` and the public main-frame wrapper select `MainFrame`, which calls
`watchdog_main_present_complete` only after finalization. `gpu_clear_display` selects `Transition`,
which records `watchdog_progress` without ending boot grace. The timeout values and CDC behavior are
unchanged.

`test_gpu_clear_watchdog` exercises the shipping `GpuState::gpu_clear_display` path with blitting
disabled only to keep the regression hermetic. Before the classification fix, the child exited 134
after the one-second steady timeout; afterward it survives 1.3 seconds under the three-second boot
budget with no STUCK output. The full Clang framework CTest passes 107/107, including
`test_watchdog`, `test_fmv_watchdog`, the new regression, and `cpp_style`. A rebuilt Crash Bash passes
11/11 CTests and its serialized product gate completes 2/2 module loads, prints `empty prims`, and
reaches MENU `0x800B5244` without a watchdog stall.
