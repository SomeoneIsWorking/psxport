---
id: C034
kind: claim
status: holds
created: 2026-08-24
tags: architecture,linkage,fps60,presentation
depends: runtime/recomp/frame_presenter.cpp#CoreFramePresentationBackend, runtime/recomp/fps60.cpp#Fps60::present_vk, runtime/recomp/fps60_game_hooks.cpp, runtime/recomp/fps60_gpu_present.cpp, tests/test_direct_runtime_no_temporal_link.cpp
---

## Claim

A direct GameRuntime links neither the concrete Fps60 product nor the game_fps60_* and gpu_fps60_* temporal helper groups.

## Evidence

The strengthened test_direct_runtime_no_temporal_link failed before the ownership split and passed afterward. Its nm -C scan observed a non-empty direct-runtime executable and found zero Fps60::, Fps60 vtable, game_fps60_*, or gpu_fps60_present_pass symbols. Clang 22.1.8 built all targets and full CTest passed 96/96, including clang-format and clang-tidy policy checks.

## What would falsify it

If the direct-runtime link test reports any concrete or helper temporal symbol, or if neutral FramePresenter or game_hooks_opt code again references a temporal-only operation.
