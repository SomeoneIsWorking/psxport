---
id: C006
kind: claim
status: holds
created: 2026-08-21
tags: watchdog,presentation,bootstrap
depends: runtime/recomp/watchdog.cpp#watchdog_progress, runtime/recomp/watchdog.cpp#watchdog_main_present_complete, runtime/recomp/native_stub.cpp#native_scea_splash, runtime/recomp/native_fmv.cpp#present_rgb555, runtime/recomp/gpu_native.cpp#GpuState::gpu_present_ex, tests/test_watchdog.cpp#main
verified_at: 2026-08-21 02:53:49
reconfirmed: 2026-08-21
---

## Claim

Bootstrap SCEA/FMV image frames reset the watchdog without ending cold-init grace. Only a completed main-VRAM presentation makes the one-way transition to steady timing, and every later heartbeat—including a later FMV—retains the steady timeout.

## Evidence

The Clang-built `build/tests/test_watchdog` passed 17/17 checks through the shipping signal/timer API: two bootstrap heartbeats each arrived after the one-second steady budget and survived under the three-second boot budget; a main presentation armed the steady alarm; and a later generic heartbeat reset that steady alarm without restoring boot grace. `build/tests/test_fmv_watchdog` passed 9/9 checks and pins both image presenters to the phase-preserving heartbeat. The main-VRAM caller reports readiness only after `frame_finalize` returns.

## What would falsify it

A SCEA/FMV image heartbeat transitions to steady timing before the main VRAM presenter completes, a main presentation is killed on the steady timeout while cold initialization is still running, or a generic heartbeat after readiness restores the boot timeout.

## Re-confirmed 2026-08-21

2026-08-21: shipping watchdog tests passed 17/17, SCEA/FMV ownership tests 9/9, and integrated Clang CTest passed 70/70; bootstrap progress preserves boot grace and later progress cannot restore it after main readiness.
