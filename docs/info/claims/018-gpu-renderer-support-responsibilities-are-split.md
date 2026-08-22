---
id: C018
kind: claim
status: holds
created: 2026-08-22
tags:
depends: runtime/recomp/gpu_native.cpp#GpuState::gp0_exec, runtime/recomp/gpu_vk.cpp#GpuVkState::present, runtime/recomp/gpu_primitive_dump.cpp#gpu_primitive_dump_finish_frame, runtime/recomp/image_writer.cpp#image_write_rgb24, tools/check_cpp_style.py
---

## Claim

GPU renderer support responsibilities are split into peer owners and both critical legacy caps are shrink-only at 4,121/4,404 lines

## Evidence

Clang 22 full build succeeded; 83/83 CTest passed including cpp_style exact caps, clang-format, clang-tidy, test_image_writer production PPM output, and psxport_smoke 8/8

## What would falsify it

if gpu_native.cpp exceeds 4,121, gpu_vk.cpp exceeds 4,404, either extracted owner is bypassed/duplicated, or the full Clang CTest fails
