---
id: 46
title: Textured raster covers Vulkan pixel centres, not PSX integer coordinates
status: resolved
symptom: Small textured primitives land one pixel off, because only the untextured pipeline carried the PSX coverage rule
tags: gpu,renderer,texture,raster,coverage,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The PSX evaluates coverage at a pixel's INTEGER coordinate; Vulkan evaluates at the fragment centre.
`tri.vert` shifts the untextured pipeline by half a native pixel to reconcile the two, but `tritex.vert`
did not, so the two pipelines carried different coverage rules. On a primitive a couple of pixels wide
that is the difference between covering a pixel and missing it, and the port then shows the feature one
pixel off with a neighbouring Gouraud/texel value.

An earlier attempt applied the same shift to the textured path and was rejected after it regressed Crash
Bash's frame-300 diff to 15,053 pixels. That attempt was wrong for two reasons, not one: it left
`psx_uv.glsl` rewinding affine UV by the fragment's offset from the pixel's top-left corner, which
double-corrects by half a pixel once the geometry itself has moved, and it was measured while the
texture-modulation rounding of issue 0045 was still present.

## Resolution

`tritex.vert` now applies the same half-native-pixel shift as `tri.vert`, and `psxUvAtIntegerPixel()`
rewinds from the native pixel's centre (`(frag % scale) + 0.5 - scale/2`) instead of its corner, so both
pipelines and the UV reconstruction express one coverage convention. At 1x the fragment centre now IS the
PSX sample point and the rewind is zero.

`gpu_vk_texture_coverage_selftest.cpp` owns the discriminator that was missing: the same narrow triangle
shape the untextured edge probe uses, textured with `raw=1` so the probe reads coverage and nothing else,
plus an interior control probe that is covered under either convention. Before the fix it reports
`edge=1882` (the seeded background) with `interior=34D4`, which separates "the raster missed this edge"
from "the draw never rasterized"; after the fix both read `34D4`. The 28/28 UV-phase matrix passes either
way, which is exactly why it could not catch this.

Measured on the consuming port at framework `db30e329` plus this change: Crash Bash's presented frames
299/300/301 fall from 99/45/24 differing pixels to 0/6/6 of 691,200 against the PSX reference. Frame 299
is exact; the frame-300 residual is one source pixel differing by one 5-bit step in blue. Framework
ctest 126/126.
