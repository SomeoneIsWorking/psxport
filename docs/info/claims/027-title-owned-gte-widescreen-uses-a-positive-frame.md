---
id: C027
kind: claim
status: holds
created: 2026-08-22
tags: widescreen,projection
depends: runtime/psx/guest_widescreen_projection.h, runtime/psx/guest_widescreen_projection.cpp, runtime/psx/render_mode.h, runtime/psx/gpu_display_mode.h, tests/test_guest_widescreen_projection.cpp, tests/test_gpu_display_mode.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:33:48
---

## Claim

Title-owned GTE widescreen uses a positive frame latch without relaxing the Native enhancement gate

## Evidence

test_guest_widescreen_projection proved 4:3/PSX/native suppression, MMX4 320->428/OFX214, and Tekken's independent 368 display/384 projection/368 draw extents; test_gpu_display_mode proved GP1 bit6 selects 368; final Clang CTest passed 88/88 and MMX4 consumer passed 7/7

## What would falsify it

if GTE guest widening enables interpolation/ires/native depth, widens before a title latch, or conflates display, projection, and draw extents

## Re-confirmed 2026-08-22

Reconfirmed after final Clang build and CTest 88/88; exact MMX4/Tekken extent controls and GP1 HRES2 opposite answer remained green; MMX4 consumer 7/7

## Re-confirmed 2026-08-22

Full Clang build and 88/88 CTests pass after exact opposite-answer guest projection controls: 320 display/project/draw widens to 428; distinct 368 display/draw widens to 492 while 384 projection widens directly to 512; Psx/Native and 4:3 remain identity. MMX4 direct consumer full Clang link and 8/8 title tests pass without Fps60/lerp/native-depth/native-producer dependencies.

## Re-confirmed 2026-08-22

Post-composition Clang CTest 90/90 passed guest-wide 320->428, 368->492, 384->512, 4:3 identity, latch, path, and display-mode controls.
