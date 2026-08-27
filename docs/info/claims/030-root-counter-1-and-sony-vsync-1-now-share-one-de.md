---
id: C030
kind: claim
status: falsified
created: 2026-08-22
tags: timing,vsync,hsync
depends: runtime/recomp/emulated_time.cpp#EmulatedTime::hSyncCount, runtime/recomp/timing.cpp#Timing::hSyncCounter, runtime/recomp/timing.cpp#Timing::vsync, runtime/recomp/mem.cpp#Core::io_read, tests/test_hsync_counter.cpp
reconfirmed: 2026-08-25
verified_at: 2026-08-25 01:16:04
falsified_on: 2026-08-27
---

## Claim

Root counter 1 and Sony VSync(1) now share one deterministic NTSC/PAL HSync phase derived from per-Game emulated time.

## Evidence

Vagrant's intact Sony VSync body reads the pointer resolving to 0x1F801110 and subtracts its saved low-16-bit baseline. The previous runtime returned zero for direct mode 1 and left that MMIO address unmapped. test_hsync_counter now passes the shipping MMIO seam, direct VSync delta/reset seam, exact 263/314 nominal field advances, intra-field line-248 progression, and zero-cadence negatives; the final Clang build and complete CTest pass 91/91 including format, clang-tidy, and structure.

## What would falsify it

A reference trace disagrees with the nominal NTSC/PAL phase, an interlaced-field consumer requires alternating line parity, either shipping seam diverges from the other, or test_hsync_counter/full CTest regresses.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 73bb5338: test_hsync_counter and the CDC/memory regression set passed in the full 100/100 CTest suite.

## Re-confirmed 2026-08-25

Fresh Clang 22 build at 64f5e181: test_hsync_counter plus the full memory/CDC regression suite passed within the complete 102/102 CTest suite.

## FALSIFIED 2026-08-27

The 2026-08-27 native-frame-loop contract removed every successful Timing::vsync mode; Sony VSync(1) now aborts at the mandatory PlatformHle trap and only root-counter MMIO remains observable.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
