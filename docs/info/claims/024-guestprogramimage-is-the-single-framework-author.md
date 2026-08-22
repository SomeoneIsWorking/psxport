---
id: C024
kind: claim
status: holds
created: 2026-08-22
tags: architecture,game-runtime,guest-program-image
depends: runtime/recomp/game_runtime.h#GameRuntime, runtime/recomp/game_iface.cpp#LegacyGameRuntimeAdapter, runtime/recomp/native_boot.cpp, runtime/recomp/overlay_router.cpp#program_image_for_routing, runtime/recomp/sync_overrides.cpp#guest_backtrace_to, tests/test_guest_program_image_ownership.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:02:18
---

## Claim

GuestProgramImage is the single framework authority for executable boot, resident-main routing, and backtrace code-range facts; direct GameRuntime inheritance no longer requires the legacy GameConfig fields for those algorithms.

## Evidence

Clang framework build and full 84/84 CTest pass; `test_game_runtime` 5/5 with 49 checks covers direct
and adapter ownership plus range validity; `test_guest_program_image_ownership` passes 43 checks and
rejects legacy-field access in all five generic consumers; `crt0_extract` shipping seam selftest
passes 59/59; independent CTR embedded consumer gate passes 87/87.

## What would falsify it

Any crt0, resident-main routing, or guest-backtrace consumer reads the legacy boot/recMain/codeScan fields; a direct runtime's image lifetime ends before a Core; or an adapter projects different facts than its legacy input.

## Re-confirmed 2026-08-22

Full Clang framework build and 84/84 CTest pass; `test_game_runtime` 5/5 with 49 checks; ownership
test 43/43 covers all five generic consumers; `crt0_extract` 59/59; CTR embedded
framework/consumer gate 87/87.

## Re-confirmed 2026-08-22

Post-commit full Clang CTest passed GuestProgramImage ownership, boot-group, routing, and backtrace-dependent contracts within 85/85.
