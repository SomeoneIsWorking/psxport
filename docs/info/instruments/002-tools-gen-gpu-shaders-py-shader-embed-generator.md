---
id: I002
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

tools/gen_gpu_shaders.py shader embed generator

## Validated by

Its hermetic selftest passed 5/5: the positive fixture compiled the exact ordered 18-entry shader
ABI, a second run was an idempotent no-op, and a shared `.glsl` dependency change rebuilt all
entries while byte-identical output kept the generated header's mtime unchanged and refreshed only
the build stamp. A forced compiler rejection and an empty PATH both produced refusals; the rejection
preserved both the existing header and the pre-failure stamp so the build must retry.

The real GNU Make CMake target was also gated in both directions: deleting the ignored generated
header caused the target's existence guard to recreate it, while the immediately repeated full
Tomba target build compiled and linked nothing. A timestamp-only `.glsl` change ran `glslc`, found
identical header bytes, refreshed only the stamp, and likewise compiled and linked nothing.

## Known failure modes

(none recorded yet)
