---
id: C017
kind: claim
status: holds
created: 2026-08-22
tags: architecture,game-runtime
depends: runtime/recomp/game_iface.cpp#psxport_install_game, runtime/recomp/core.cpp#Core, runtime/recomp/game.h#Game, runtime/recomp/game_runtime.h#GameRuntime
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:02:18
---

## Claim

The installed derived GameRuntime owns Core context lifecycle and creates one fully-wired FrameDriver and TaskScheduler per Game; the bounded legacy pair still delegates during migration.

## Evidence

scratch/build-game-runtime/tests/test_game_runtime: 3/3 cases, 21 checks, 0 failed; scratch/bin/psxport_smoke: 8/8 checks with psxport_game_config()==nullptr and psxport_game_hooks()==nullptr; full CTest: 81/82 passed with the only failure an untouched pre-existing pad_input.h format violation.

## What would falsify it

A Core bypasses GameRuntime lifecycle, a Game factory runs before core.game/subsystem wiring, a direct-runtime smoke exposes non-null legacy views, or a legacy consumer no longer receives its installed context/boot callbacks.

## Re-confirmed 2026-08-22

scratch/build-game-runtime-main/tests/test_game_runtime: 4/4 cases, 30 checks, 0 failed after integration in the main checkout, including a derived LegacyGameRuntimeAdapter overriding one behavior while retaining legacy context/config; scratch/bin/psxport_smoke: 8/8 with both legacy views null; Clang-tidy clean on all 5 touched TUs. The isolated full CTest was 81/82, with the sole failure the then-untouched pad_input.h:31 formatting.

## Re-confirmed 2026-08-22

Reverified after derived runtime integration: test_game_runtime passes in the full 83/83 Clang framework CTest; touched runtime files pass clang-format and clang-tidy.

## Re-confirmed 2026-08-22

Reverified after the first typed fact slice: full Clang framework build and CTest pass 84/84;
`test_game_runtime` passes 5/5 cases and 49 checks, including direct `GuestProgramImage` ownership,
range validity, and the bounded adapter projection; independent CTR embedded consumer CTest passes
87/87.

## Re-confirmed 2026-08-22

Post-commit full Clang CTest passed GameRuntime and guest-program ownership contracts within 85/85.
