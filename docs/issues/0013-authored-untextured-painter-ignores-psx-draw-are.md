---
id: 13
title: Authored untextured painter ignores PSX draw area
status: resolved
symptom: Native Spyro widescreen shows a black upper-right wedge and noisy top/bottom guard rows after world geometry and painter order are correct
tags: gpu,renderer,painter,clip,spyro
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

The custom untextured authored-painter fragment shader consumed color, gouraud, and dither inputs but omitted the per-item `v_da` draw-area rectangle already carried by the vertex path. Fragments outside the guest primitive's authored PSX draw area therefore wrote into the eight guard rows.

## Resolution

`painter_tri.frag` now derives the native pixel from `gl_FragCoord` and the active scale, then discards outside the inclusive per-item draw area before dithering. A real Vulkan selftest stages one triangle across a smaller draw area and proves an outside probe remains `0000` while an inside probe becomes `001F`. Full Clang framework CTest passes 83/83.
