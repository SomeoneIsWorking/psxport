---
id: C006
kind: claim
status: holds
created: 2026-08-21
tags: watchdog,presentation,bootstrap
depends: runtime/psx/gpu_native.cpp#GpuState::gpu_present_ex
verified_at: 2026-08-24 23:01:25
reconfirmed: 2026-08-27
---

## Claim

Bootstrap SCEA/FMV image frames and explicit transition clears reset the watchdog without ending cold-init grace. Only a completed main-VRAM presentation makes the one-way transition to steady timing, and every later heartbeat—including a later FMV—retains the steady timeout.

## Evidence

The Clang-built `build/tests/test_watchdog` passes 17/17 checks through the shipping signal/timer API: two bootstrap heartbeats each arrive after the one-second steady budget and survive under the three-second boot budget; a main presentation arms the steady alarm; and a later generic heartbeat resets that steady alarm without restoring boot grace. `build/tests/test_fmv_watchdog` passes 9/9 and pins both image presenters to the phase-preserving heartbeat. `build/tests/test_gpu_clear_watchdog` reaches the shipping `gpu_clear_display` helper and proves its transition clear survives beyond the steady budget without claiming main readiness. The main-frame caller reports readiness only after `frame_finalize` returns.

## What would falsify it

A SCEA/FMV image heartbeat or transition clear selects steady timing before the main VRAM presenter completes, a main presentation is killed on the steady timeout while cold initialization is still running, or a generic heartbeat after readiness restores the boot timeout.

## Re-confirmed 2026-08-21

Shipping watchdog tests passed 17/17 and SCEA/FMV ownership tests passed 9/9; bootstrap progress
preserves boot grace and later progress cannot restore it after main readiness. Post-integration
standalone Clang CTest passed test_watchdog as part of 74/74; the recorded real Tomba LOGO.STR run
remains the consumer positive.

## Re-confirmed 2026-08-22

Post-extraction Clang CTest passed 83/83, including test_watchdog 17/17 and test_fmv_watchdog 9/9; gpu_present_ex retained the main-VRAM completion call after primitive-dump/image-writer ownership moved out of gpu_native.cpp.

## Re-confirmed 2026-08-22

Reverified after renderer/runtime changes: test_fmv_watchdog and test_watchdog pass inside the full 83/83 Clang framework CTest.

## Re-confirmed 2026-08-22

Post-composition Clang CTest 90/90 passed test_fmv_watchdog and cpp_style after gpu_native/presentation changes.

## Re-confirmed 2026-08-24

2026-08-24 Clang full framework build and CTest 96/96 passed after 7bd24f2b; test_watchdog and the cold-init watchdog path remain green.

## Re-confirmed 2026-08-27

The new production-seam `test_gpu_clear_watchdog` showed both answers: before the typed completion
fix, the bootstrap clear ended grace and the child exited 134 at the steady alarm; afterward the same
shipping helper survives under boot grace. Full Clang CTest passes 107/107. A rebuilt Crash Bash
consumer passes its deterministic MENU boundary without STUCK, confirming the previous product
alarm was the transition-classification bug rather than an allowed bootstrap heartbeat ending grace.
