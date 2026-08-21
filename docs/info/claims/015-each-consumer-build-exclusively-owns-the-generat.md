---
id: C015
kind: claim
status: holds
created: 2026-08-21
tags: cmake,gpu,shader,concurrency
depends: cmake/gpu_shaders.cmake#psxport_add_gpu_shaders, tools/gen_gpu_shaders.py#validate_output_owner, tests/test_gpu_shader_build_ownership.py#main
reconfirmed: 2026-08-21
verified_at: 2026-08-21 14:07:17
---

## Claim

Each consumer build exclusively owns the generated SDL_GPU shader header it compiles.

## Evidence

The shipping generator selftest runs two outputs concurrently, removes one owner, proves the peer
survives byte-identical, regenerates the removed output, and refuses the legacy source-tree path.
The CMake integration gate configures two real Ninja build directories with the framework nested
under `add_subdirectory`. Both Clang targets compile a source that includes the namespaced header,
and clean A leaves B intact. Removing the exact target include owner makes compilation fail; the
legacy shared-BYPRODUCT fixture independently produces the missing-peer-header answer.

## What would falsify it

Cleaning or regenerating one consumer build can remove, replace, or shadow the shader header
compiled by another build; a renderer resolves a source-tree legacy header before the build-owned
header; or the shipping CMake module emits the header outside its caller's binary directory.

## Re-confirmed 2026-08-21

Nested `add_subdirectory` Clang ownership gate passes 4/4, removing the exact target include owner
fails compilation, the legacy shared BYPRODUCT reproduces peer deletion, and framework CTest passes
81/81. Fresh nested Crash Bash compiles both renderer TUs and links; Crash 1 additionally passes its
four canonical oracle-call boundaries at 34/34 each and 9/9 classifier selftest.
