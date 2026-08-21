---
id: C015
kind: claim
status: holds
created: 2026-08-21
tags: cmake,gpu,shader,concurrency
depends: cmake/gpu_shaders.cmake#psxport_add_gpu_shaders, tools/gen_gpu_shaders.py#validate_output_owner, tests/test_gpu_shader_build_ownership.py#main
reconfirmed: 2026-08-21
verified_at: 2026-08-21 13:58:09
---

## Claim

Each consumer build exclusively owns the generated SDL_GPU shader header it compiles.

## Evidence

The shipping generator selftest runs two outputs concurrently, removes one owner, proves the peer
survives byte-identical, regenerates the removed output, and refuses the legacy source-tree path.
The CMake integration gate configures two real Ninja build directories through the shipping module,
builds both shader targets concurrently, and proves clean A leaves B intact. Its legacy shared-
BYPRODUCT fixture produces the opposite missing-header answer.

## What would falsify it

Cleaning or regenerating one consumer build can remove, replace, or shadow the shader header
compiled by another build; a renderer resolves a source-tree legacy header before the build-owned
header; or the shipping CMake module emits the header outside its caller's binary directory.

## Re-confirmed 2026-08-21

Clang CTest 80/80; generator 7/7; shipping CMake two-build concurrency/peer-clean integration 3/3 including legacy shared-BYPRODUCT opposite; full psxport build compiled both renderer consumers.

## Re-confirmed 2026-08-21

Post-integration Clang build passed both renderer consumers; generator selftest passed 7/7, two-build CMake ownership gate passed 3/3 including the legacy peer-clean opposite, and full CTest passed 81/81.
