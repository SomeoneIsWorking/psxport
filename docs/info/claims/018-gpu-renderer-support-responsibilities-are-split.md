---
id: C018
kind: claim
status: holds
created: 2026-08-22
tags:
depends: runtime/recomp/gpu_vk.cpp#GpuVkState::present, runtime/recomp/gpu_painter.cpp#GpuVkState::painter_command
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:33:39
---

## Claim

GPU renderer support responsibilities are split into peer owners and both critical legacy caps are shrink-only at 4,121/4,404 lines

## Evidence

Clang 22 full build succeeded; 83/83 CTest passed including cpp_style exact caps, clang-format, clang-tidy, test_image_writer production PPM output, and psxport_smoke 8/8

## What would falsify it

if gpu_native.cpp exceeds 4,121, gpu_vk.cpp exceeds 4,404, either extracted owner is bypassed/duplicated, or the full Clang CTest fails

## Re-confirmed 2026-08-22

Reverified after authored replay and draw-area work: gpu_vk.cpp shrank to 4,402 lines, painter lifecycle/staging/discriminator remain in gpu_painter.{h,cpp}, full Clang CTest passes 83/83, and the real Vulkan draw-area discriminator passes both answers.

## Re-confirmed 2026-08-22

Post-commit baseline corrected after 0f808dc9: that commit is the already-recorded authored draw-area extraction/discriminator change, not a later unverified mutation. Current full Clang framework CTest passes 84/84, including cpp_style structure caps, format, and tidy.

## Re-confirmed 2026-08-22

Post-composition cpp_style passed inside Clang CTest 90/90; structure caps and peer renderer ownership remained enforced.

## Re-confirmed 2026-08-22

Post-hook-fix Clang CTest 90/90 passed cpp_style, structure and all renderer ownership controls after GpuVkState::present was routed through the guarded fade accessor.

## Re-confirmed 2026-08-24

Post-policy Clang build and full 93/93 CTest passed cpp_style/structure; renderer ownership change stayed within present policy and the critical legacy file cap did not grow

## Re-confirmed 2026-08-25

X4 exact Gte capture renders 29921/691200 nonblack pixels after the page-persistence fix; focused guest composite/ownership/backdrop tests all pass in the current Clang build.
