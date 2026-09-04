---
id: C021
kind: claim
status: holds
created: 2026-08-22
tags: gpu,renderer,painter,order
depends: runtime/psx/shaders_gpu/painter_tri.frag#main, runtime/psx/gpu_painter.cpp#gpu_vk_painter_stage_draw_area_selftest
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:00:09
---

## Claim

Authored painter replay domains preserve cross-object guest OT order through the shipping presentation path within each physical flush

## Evidence

Framework planner tests merge objects from distinct captured queues, enforce the shared replay comparator, and falsify duplicate keys, unordered world faces, mixed policies, object/domain aliasing, and mixed flush epochs; GPU staging proves object transitions do not coalesce; full Clang CTest passed 83/83; a real Spyro stage-13 presentation reported one 782-command authored range across three objects and restored the guest foreground/background ordering.

## What would falsify it

A shipping frame bypasses planPainterItemStream, items from different physical flushes are regrouped, or a guest/reference OT stream disagrees with the planned cross-object command order.

## Re-confirmed 2026-08-22

Reverified after per-flush local-rank correction: planner/staging tests and full Clang framework CTest pass 83/83; touched 7/7 compile-backed C++ TUs pass clang-tidy; real Spyro stage-13 output has coherent cross-object foreground/background ordering.

Reverified after restoring the untextured painter's per-item draw-area clip: the real Vulkan
discriminator leaves the outside probe `0000` and paints the inside probe `001F`; the full Clang
framework CTest still passes 83/83. This distinguishes correct authored replay from geometry leaking
outside its guest clip rectangle.

## Re-confirmed 2026-08-22

Real Vulkan authored-painter draw-area discriminator preserves outside=0000 and paints inside=001F; full Clang framework CTest passes 83/83.

## Re-confirmed 2026-08-22

Post-commit baseline corrected after 0f808dc9: the previously recorded real Vulkan outside=0000/inside=001F discriminator and Spyro authored-order result cover that exact draw-area change. Current planner/staging checks and full Clang framework CTest pass 84/84.
