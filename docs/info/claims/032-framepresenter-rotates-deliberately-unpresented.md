---
id: C032
kind: claim
status: holds
created: 2026-08-24
tags:
depends: runtime/recomp/frame_presenter.cpp#FramePresenter::commitUnpresented, runtime/recomp/pad_input.cpp#Pad::serviceFrame
---

## Claim

FramePresenter rotates deliberately unpresented fields without output, and Pad keeps slot 1 absent unless the game explicitly opts in

## Evidence

Clang full build and CTest 95/95; test_frame_presenter_unpresented proves no emit/present/pace/diagnostic plus capture rotation and next-frame isolation; test_pad_slot1_policy proves absent default and connected packet positives

## What would falsify it

either unpresented fields reach an output path or a default Pad instance reports slot 1 present
