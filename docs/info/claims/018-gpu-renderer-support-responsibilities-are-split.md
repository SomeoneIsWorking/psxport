---
id: C018
kind: claim
status: holds
created: 2026-08-22
tags:
depends: runtime/recomp/gpu_vk.cpp#GpuVkState::present, runtime/recomp/gpu_painter.cpp#GpuVkState::painter_command
reconfirmed: 2026-08-22
verified_at: 2026-08-22 17:01:35
---

## Claim

GPU renderer support responsibilities are split into peer owners and both critical legacy caps are shrink-only at 4,121/4,404 lines

## Evidence

Clang 22 full build succeeded; 83/83 CTest passed including cpp_style exact caps, clang-format, clang-tidy, test_image_writer production PPM output, and psxport_smoke 8/8

## What would falsify it

if gpu_native.cpp exceeds 4,121, gpu_vk.cpp exceeds 4,404, either extracted owner is bypassed/duplicated, or the full Clang CTest fails

## Re-confirmed 2026-08-22

Reverified after authored replay and draw-area work: gpu_vk.cpp shrank to 4,402 lines, painter lifecycle/staging/discriminator remain in gpu_painter.{h,cpp}, full Clang CTest passes 83/83, and the real Vulkan draw-area discriminator passes both answers.
