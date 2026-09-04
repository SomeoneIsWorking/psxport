---
id: C019
kind: claim
status: holds
created: 2026-08-22
tags:
depends: runtime/psx/painter_object_layer.cpp#buildPainterObjectPlan, runtime/psx/gpu_painter.cpp#GpuVkState::painter_command
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:00:09
---

## Claim

Painter objects preserve mixed opaque and semitransparent authored command order with exact PSX blend semantics

## Evidence

test_painter_object_layer accepts and preserves semi/blend metadata; test_gpu_painter_staging proves semi commands never coalesce; full Clang framework CTest passes 83/83; production Vulkan selftest passes 16/16 semitrans equations, painter color/depth composite, mixed stream, dither, and resize.

## What would falsify it

A painter object produces a different packed color or depth than guest authored-order replay for any supported opaque/semitransparent command stream or ABR mode.

## Re-confirmed 2026-08-22

Reverified after cross-object replay and draw-area clip: planner/staging tests and full Clang CTest pass 83/83; the real Vulkan mixed-material stream remains 6/6 and the draw-area outside/inside discriminator passes.

## Re-confirmed 2026-08-22

Post-commit baseline corrected after 0f808dc9: the already-recorded mixed-material and draw-area discriminators cover that exact change. Current painter planner/staging tests and full Clang framework CTest pass 84/84.
