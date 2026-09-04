---
id: C025
kind: claim
status: holds
created: 2026-08-22
tags: presentation,runtime
depends: runtime/psx/frame_presenter.h, runtime/psx/frame_presenter.cpp, runtime/psx/game.h, runtime/psx/game_runtime.h, runtime/psx/game_runtime.cpp, tests/test_direct_runtime_no_temporal_link.cpp, tests/test_frame_presenter_contract.cpp
reconfirmed: 2026-08-24
verified_at: 2026-08-24 23:01:26
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

## Re-confirmed 2026-08-24

Post-policy full 93/93 CTest passed test_direct_runtime_no_temporal_link; nm-based concrete Fps60 absence and neutral one-present behavior remained green after adding the required runtime method

## Re-confirmed 2026-08-24

2026-08-24 Clang full framework build and CTest 96/96 passed after 7bd24f2b; test_direct_runtime_no_temporal_link, test_frame_presenter_contract, and X4's linked-binary dependency gate all pass.
