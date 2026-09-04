---
id: C031
kind: claim
status: holds
created: 2026-08-24
tags: renderer,inheritance,vram
depends: runtime/psx/game_runtime.cpp#game_guest_vram_is_picture, runtime/psx/gpu_vk.cpp#GpuVkState::present, runtime/psx/guest_vram_composite_policy.h, tests/test_guest_vram_composite_policy.cpp
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:33:40
---

## Claim

Guest-VRAM picture ownership is a required per-Game runtime policy, and the persistent composite rebuilds on both ownership transitions

## Evidence

Clang build and full 93/93 CTest gate passed; production-policy tests exercised cold/stable state, native-to-guest full-upload intent, guest-to-native invalidation, per-instance isolation, per-Game runtime answers, legacy projection, and old renderer-read rejection; standalone smoke passed 8/8

## What would falsify it

if a renderer path reads the legacy static backdrop bit, a missing runtime receives an implicit ownership answer, or either ownership transition can reuse a composite built under the opposite policy

## Re-confirmed 2026-08-25

test_guest_vram_composite_policy, test_guest_vram_policy_ownership, and test_present_backdrop_clear all pass after Gte page persistence was added as an independent ownership policy.
