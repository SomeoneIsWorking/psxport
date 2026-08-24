---
id: C032
kind: claim
status: holds
created: 2026-08-24
tags:
depends: runtime/recomp/frame_presenter.cpp#FramePresenter::commitUnpresented, runtime/recomp/pad_input.cpp#Pad::serviceFrame
reconfirmed: 2026-08-24
verified_at: 2026-08-24 23:01:26
---

## Claim

FramePresenter rotates deliberately unpresented fields without output, and Pad keeps slot 1 absent unless the game explicitly opts in

## Evidence

Clang full build and CTest 96/96; test_frame_presenter_unpresented proves no emit/present/pace/diagnostic plus capture rotation and next-frame isolation; test_pad_slot1_policy proves absent default and connected packet positives

## What would falsify it

either unpresented fields reach an output path or a default Pad instance reports slot 1 present

## Re-confirmed 2026-08-24

2026-08-24 Clang full framework build and CTest 96/96 passed after 7bd24f2b; test_frame_presenter_unpresented and test_pad_slot1_policy both pass.
