---
id: C039
kind: claim
status: holds
created: 2026-08-25
tags: repl,warp,presentation,frame-boundary
depends: runtime/recomp/native_boot.cpp#native_step_frame, runtime/recomp/native_boot.cpp#apply_armed_dev_warp, runtime/recomp/standalone_frame_boundary.h#standalone_frame_boundary, tests/test_standalone_frame_boundary.cpp
---

## Claim

A standalone cold dev warp is a frame-boundary transaction: present the pending old-scene capture,
begin the new capture epoch, apply the game-owned warp, then run destination guest work. SBS owns its
separate warp timing and never enters this standalone phase.

## Evidence

The extracted shipping-order regression was RED on the old loop: 0/2 cases passed and both failed at
the second event because `apply-warp` preceded `present-pending`. It is GREEN at 2/2 cases and 10 checks
for both armed and unarmed paths. A Clang 22 build compiled all of libpsxport including
`native_boot.cpp`; the focused CTest, clang-format, clang-tidy, and diff checks pass. In the combined
Tomba! 2 runtime log `Tomba2Engine/scratch/logs/gate-run-consumed-flush-20260825.log`, f3015 presents
the full 168-item pending old scene before the warp is applied, then f3016 begins the destination
capture with no stale old-scene items.

## What would falsify it

The standalone loop applies an armed warp before consuming its pending frame, destination guest work
runs before the warp, the unarmed path changes phase ordering, or SBS begins servicing the standalone
REPL warp phase.
