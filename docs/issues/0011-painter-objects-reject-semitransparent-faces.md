---
id: 11
title: Painter objects reject semitransparent faces
status: resolved
symptom: A direct world producer with mixed opaque and semitransparent faces fails painter planning or cannot preserve authored blend order
tags: gpu,renderer,painter,semitransparency,order
created: 2026-08-22
updated: 2026-08-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Dead end (2026-08-22)
Removing the planner's SemiTransparent refusal alone is invalid: the painter target was cleared RG8 and its pipelines only overwrote, so a semi face had neither the current canvas destination nor a hardware blend stage.

### Note (2026-08-22)
Root cause: painter planning carried material class but discarded semi/blend state, while GPU local replay started from black and only had opaque RG8 overwrite pipelines. The fix preserves semi/blend per command, seeds each object's packed target from the current canvas, and performs decode -> blend-mode-specific fixed-function blend -> encode at every semi command before continuing authored replay.

### Resolution (2026-08-22)
Root cause fixed generically: painter planning preserved neither semitransparency metadata nor destination-dependent blend execution. Painter commands now retain semitransparency/blend mode; each painter object seeds its packed local target from the current canvas, and every semitransparent command decodes the immediately preceding packed result to RGBA16F, uses the exact PSX hardware blend equation with painter depth-always/write semantics, then re-encodes before authored-order replay continues. Verified by planner/staging tests, Clang build, 83/83 CTest, cpp_style/tidy, and production Vulkan selftests including 16/16 semitrans equations and mixed painter stream PASS.
