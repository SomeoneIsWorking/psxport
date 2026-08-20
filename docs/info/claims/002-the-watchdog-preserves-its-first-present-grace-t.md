---
id: C002
kind: claim
status: falsified
created: 2026-08-21
tags: watchdog,presentation
depends: runtime/recomp/watchdog.cpp#watchdog_present_complete, runtime/recomp/gpu_native.cpp#GpuState::gpu_present_ex, tests/test_watchdog.cpp#main
reconfirmed: 2026-08-21
verified_at: 2026-08-21 02:16:03
---

## Claim

The watchdog preserves its first-present grace through cold GPU initialization and arms the steady-state timeout only after a presentation completes; a later presentation stall still terminates through the real watchdog path.

## Evidence

2026-08-21 build/tests/test_watchdog passed its first-present positive case after 1.3 s with steady=1 s and boot=3 s, and its post-completion negative case exited 134 with the STUCK diagnostic; full Clang CTest passed 67/67.

## What would falsify it

A pre-completion first present is killed at the steady timeout, or a post-completion presentation stall survives beyond the steady timeout.

## Re-confirmed 2026-08-21

2026-08-21 re-confirmed after the completed implementation: build/tests/test_watchdog passed the real first-present positive and post-completion alarm/exit negative; full Clang CTest passed 67/67; Spyro gate passed 14/14 at 3,000 fields where the old entry-time arming had exited 134 in cold SDL initialization.

## Falsified 2026-08-21

The test treated every completed presentation as proof that all cold GPU initialization was finished. Tomba's SCEA/FMV image presenter can complete before the main VRAM presenter allocates its per-Game targets; its heartbeat therefore armed the steady 15-second timeout and two contended runs exited 134 inside the first main-target allocation. C006 replaces this claim with the explicit two-phase contract.
