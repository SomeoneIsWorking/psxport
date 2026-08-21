---
id: I002
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

tools/gen_gpu_shaders.py shader embed generator

## Validated by

Its hermetic selftest passes 7/7: the positive fixture compiles the exact ordered 18-entry shader
ABI, a second run was an idempotent no-op, and a shared `.glsl` dependency change rebuilt all
entries while byte-identical output kept the generated header's mtime unchanged and refreshed only
the build stamp. A forced compiler rejection and an empty PATH both produced refusals; the rejection
preserved both the existing header and the pre-failure stamp so the build must retry.

The concurrency cases generate two build-owned outputs simultaneously, remove one as a modeled
consumer clean, regenerate it, and prove the peer remains byte-identical. The opposite source-tree
destination is refused.

`tests/test_gpu_shader_build_ownership.py` independently includes the shipping CMake module in two
Ninja build directories, builds both targets concurrently, and proves cleaning A removes only A's
header. Its mutated legacy fixture registers one source-tree header as both builds' BYPRODUCT and
reproduces the missing peer header after cleaning A: 3/3 with both answers.

## Known failure modes

The former 5/5 selftest covered only one build directory and therefore could not detect cross-build
BYPRODUCT deletion. Concurrency ownership requires the CMake integration gate as well as generator
atomicity.
