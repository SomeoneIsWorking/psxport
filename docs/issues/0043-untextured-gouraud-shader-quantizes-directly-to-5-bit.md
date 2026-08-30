---
id: 43
title: Untextured Gouraud shader quantizes directly to 5-bit
status: resolved
symptom: Native model gradients differ by one 5-bit step even when packet SXY, vertex colors, order, and dither-off state match retail
tags: gpu,renderer,gouraud,dither,quantization,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The ordinary untextured Vulkan fragment shader interpolated normalized vertex colors and rounded them
directly to 5-bit. The PSX instead rounds the interpolated channel to 8-bit, optionally applies its
4x4 dither matrix for Gouraud polygons, and then truncates to 5-bit. The ordinary vertex staging path
also discarded the queue item's Gouraud and DTD state even though `TriVtx` already carried both fields.
The authored-painter shader used the correct sequence, which initially hid the ordinary-path defect.

## Resolution

The ordinary path now preserves per-item Gouraud/DTD state and shares the PSX `round8 -> optional
dither -> truncate5` sequence. A production-path GPU discriminator uses a measured legal G3 triangle:
before the fix it emitted `1C04` instead of `1803`; after the fix it emits `1803` and independently
proves the negative and positive dither cells as `3DEF` and `4210`.
