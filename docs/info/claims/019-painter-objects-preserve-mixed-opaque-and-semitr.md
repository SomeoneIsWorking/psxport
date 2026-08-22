---
id: C019
kind: claim
status: holds
created: 2026-08-22
tags:
depends: runtime/recomp/painter_object_layer.cpp#buildPainterObjectPlan, runtime/recomp/gpu_painter.cpp#GpuVkState::painter_command, runtime/recomp/gpu_vk.cpp#render_geom
---

## Claim

Painter objects preserve mixed opaque and semitransparent authored command order with exact PSX blend semantics

## Evidence

test_painter_object_layer accepts and preserves semi/blend metadata; test_gpu_painter_staging proves semi commands never coalesce; full Clang framework CTest passes 83/83; production Vulkan selftest passes 16/16 semitrans equations, painter color/depth composite, mixed stream, dither, and resize.

## What would falsify it

A painter object produces a different packed color or depth than guest authored-order replay for any supported opaque/semitransparent command stream or ABR mode.
