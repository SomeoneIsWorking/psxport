---
id: 9
title: Shared source-tree shader byproduct is deleted by peer consumer builds
status: resolved
symptom: gpu_vk.cpp fails to compile because gpu_vk_shaders.h is absent even though gen_gpu_shaders reports built during parallel game builds
tags: cmake,gpu,shader,generator,concurrency,build-ownership
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

Every independent consumer build registered the same ignored source-tree `runtime/recomp/gpu_vk_shaders.h` as its CMake `BYPRODUCT`. Each build owned a different freshness stamp but wrote, read, and cleaned one shared header. Cleaning or regenerating one build could therefore remove the header after another build had completed `gen_gpu_shaders` and before its compiler opened `gpu_vk.cpp`. Atomic replacement did not protect against a different build deleting the shared byproduct.

## Resolution

Each consumer now generates `psxport_generated/gpu_vk_shaders.h` under its own `CMAKE_CURRENT_BINARY_DIR`, passes that explicit path to the generator, and includes the namespaced build-tree path. The generator refuses the former source-tree destination. Its build-local stamp retains byte-stable no-op behavior.

The 7/7 hermetic generator gate runs two build-owned outputs concurrently, removes one as a modeled peer clean, proves the other remains byte-identical, regenerates the removed owner, and rejects the legacy shared source path. A real two-Ninja-build check produced identical headers; cleaning build A removed only A while build B retained and compiled its header.

## Verification

Clang 22.1.8 configured the framework and both independent Ninja trees. The full framework build
compiles both `gpu_vk.cpp` and `rmlui_render_gpu.cpp` against the build-owned header. CTest passes
80/80, including the 7/7 generator selftest and 3/3 CMake ownership integration gate. The normal
C++ policy format-checks 306 first-party files and clang-tidy checks 163/166 compile-backed
first-party translation units; its 8/8 refusal selftest also passes.
