---
id: C025
kind: claim
status: holds
created: 2026-08-22
tags: presentation,runtime
depends: runtime/recomp/frame_presenter.h, runtime/recomp/frame_presenter.cpp, runtime/recomp/game.h, runtime/recomp/game_runtime.h, runtime/recomp/game_runtime.cpp, tests/test_direct_runtime_no_temporal_link.cpp, tests/test_frame_presenter_contract.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:33:47
---

## Claim

A direct GameRuntime owns the ordinary frame fence without constructing or linking Fps60

## Evidence

test_direct_runtime_no_temporal_link inspects its own final symbol table and found no Fps60 symbols; test_frame_presenter_contract drove capture, one present, diagnostic, explicit field pace, ledger reconcile/reset; final Clang CTest passed 88/88

## What would falsify it

if a direct-runtime executable links an Fps60 symbol or neutral commit needs temporal history

## Re-confirmed 2026-08-22

Reconfirmed after final Clang build and CTest 88/88; direct executable symbol-table falsifier remained free of Fps60

## Re-confirmed 2026-08-22

Full Clang build and 88/88 CTests pass; the direct-runtime neutral fence test invokes FramePresenter::commit and proves its linked image contains no Fps60 symbol or vtable, while MMX4's direct consumer full-links and passes 8/8 without temporal dependencies.

## Re-confirmed 2026-08-22

Post-composition Clang CTest 90/90 passed direct-runtime no-temporal link and neutral FramePresenter shipping-state controls.
