---
id: 22
title: Static VRAM-backdrop policy leaks texture rows beneath native frames
status: investigating
symptom: Spyro 1 widescreen title frame shows multicolour corrupt bands above and below otherwise correct native geometry
tags: runtime,renderer,gameconfig,inheritance,spyro,vram,widescreen
created: 2026-08-22
updated: 2026-08-24
---

## Root cause

The renderer read `GameConfig::preserveVramBackdrop` as an immutable title-wide fact. Spyro needs that value true while boot logos are upload-only guest VRAM, but its fully native stage-13 producer covers only the measured display viewport. Keeping raw VRAM underneath that native frame exposes texture-atlas rows at y=0..7 and y=232..239. A same-build diagnostic with the bit false made both bands uniformly black, while historical boot evidence proves false globally would erase the logos.

## Implementation under verification

`GameRuntime::guestVramIsPicture(const Game&)` is the required per-frame inherited policy owner. GPU present and SBS readback ask the runtime through a checked query and never read `GameConfig`; every direct runtime must state its policy explicitly. `LegacyGameRuntimeAdapter` alone projects the old static bit for unmigrated consumers. Mixed guest/native titles override the policy from their classified render ownership state.

The persistent host composite also owns a per-`Game` latch recording the policy under which it was last built. Either ownership transition invalidates the composite. Native-to-guest marks all VRAM for upload before rebuilding; guest-to-native rebuilds without the old guest backdrop. This prevents a correct per-frame answer from reusing a composite built under the opposite policy.

## Verification

Framework unit coverage must prove per-`Game` answers, both composite-ownership transitions, instance isolation, legacy projection, and the absence of renderer reads of `preserveVramBackdrop`. The issue remains open until Spyro supplies fresh same-build guest-logo and native-widescreen captures and they are visually inspected.
